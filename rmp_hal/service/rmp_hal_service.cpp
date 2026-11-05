// SPDX-License-Identifier: MIT
// l01ktv_rmp_hal: PROTECTED_ES producer の vendor AIDL HAL service。
// IRmpProducer を実装し、Cコア(rmp_producer_core)をワーカースレッドで駆動する。
// 層2(player↔producer)の exec+frame_ipc+FD直結を置換し、#A境界を hal_attribute 経由の binder で越える。
// 境界: librmp opaque / 保護payload非map / 鍵・設定への権限なし / RFは rf.lock単一所有(Cコア内)。
// ログ(logcat tag l01ktv_rmp_hal): session の開始/状態/終了とエラーコードのみ。ECM・鍵・payload は出さない。
#define LOG_TAG "l01ktv_rmp_hal"
#include <aidl/vendor/l01k/tv/rmp/BnRmpProducer.h>
#include <aidl/vendor/l01k/tv/rmp/IRmpFrameCallback.h>
#include <aidl/vendor/l01k/tv/rmp/RmpTuneParams.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rmp_producer_core.h"

using aidl::vendor::l01k::tv::rmp::BnRmpProducer;
using aidl::vendor::l01k::tv::rmp::IRmpFrameCallback;
using aidl::vendor::l01k::tv::rmp::RmpTuneParams;
using ndk::ScopedAStatus;
using ndk::ScopedFileDescriptor;

namespace {

class RmpProducer : public BnRmpProducer {
  public:
    ScopedAStatus start(const RmpTuneParams& params,
                        const std::vector<ScopedFileDescriptor>& secureBuffers,
                        const std::shared_ptr<IRmpFrameCallback>& cb,
                        int64_t* _aidl_return) override {
        std::lock_guard<std::mutex> lk(mLifecycle);
        if (mWorker.joinable()) {
            ALOGW("start rejected: session already active");
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        if (!cb || secureBuffers.size() < 2) {
            ALOGW("start rejected: bad arguments (buffers=%zu)", secureBuffers.size());
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }

        std::vector<int> fds;
        fds.reserve(secureBuffers.size());
        for (const auto& pfd : secureBuffers) fds.push_back(pfd.get());  // core が dup して保持

        mCb = cb;
        mAckQueue.clear();
        mFrames = 0;
        mAcks = 0;
        ALOGI("session start ch=%d buffers=%zu", params.channel, secureBuffers.size());

        rmp_core_cb ccb = {&RmpProducer::onStateThunk, &RmpProducer::onFrameThunk, this};
        int err = 0;
        mCore = rmp_core_start(params.channel, params.station, params.ecmPid, params.videoPid,
                               static_cast<unsigned>(params.bufferSize), fds.data(),
                               static_cast<unsigned>(fds.size()), &ccb, &err);
        if (!mCore) {
            mCb.reset();
            // err: 11=busy, 2=引数, 12=その他。ServiceSpecific で返す。
            ALOGE("session start failed err=%d", err);
            return ScopedAStatus::fromServiceSpecificError(err);
        }
        mRunning = true;
        mWorker = std::thread(&RmpProducer::workerLoop, this);
        // client(player/bridge)が stop せずに死んでも RF/rf.lock/保護バッファを握り続けないよう、
        // callback binder の死亡で stop と同じ解放を行う(V5)。
        if (AIBinder_linkToDeath(cb->asBinder().get(), mDeath.get(), this) != STATUS_OK) {
            ALOGW("linkToDeath failed; session is released only by stop");
        }
        *_aidl_return = kSessionId;
        return ScopedAStatus::ok();
    }

    ScopedAStatus ack(int64_t sessionId, int32_t index) override {
        if (sessionId != kSessionId || index < 0) return ScopedAStatus::ok();  // 冪等的に無視
        std::lock_guard<std::mutex> lk(mAckMu);
        mAckQueue.push_back(static_cast<uint32_t>(index));
        return ScopedAStatus::ok();
    }

    ScopedAStatus stop(int64_t sessionId) override {
        std::lock_guard<std::mutex> lk(mLifecycle);
        if (sessionId != kSessionId) return ScopedAStatus::ok();  // 冪等
        ALOGI("session stop requested");
        stopLocked();
        return ScopedAStatus::ok();
    }

  private:
    static constexpr int64_t kSessionId = 1;

    // mLifecycle を保持して呼ぶ。worker が rmp_core_stop + onEnded を実施する。
    void stopLocked() {
        mRunning = false;
        if (mWorker.joinable()) mWorker.join();
        if (mCb) AIBinder_unlinkToDeath(mCb->asBinder().get(), mDeath.get(), this);
        mCb.reset();
    }

    static void onClientDied(void* cookie) {
        auto* self = static_cast<RmpProducer*>(cookie);
        std::lock_guard<std::mutex> lk(self->mLifecycle);
        if (!self->mWorker.joinable()) return;  // stop 済み
        ALOGW("client died; releasing session");
        self->stopLocked();
    }

    void workerLoop() {
        while (mRunning.load()) {
            // player からの ACK を同一スレッドでコアへ反映(REUSE)。
            {
                std::vector<uint32_t> drained;
                {
                    std::lock_guard<std::mutex> lk(mAckMu);
                    drained.swap(mAckQueue);
                }
                for (uint32_t idx : drained) rmp_core_ack(mCore, idx);
                mAcks += drained.size();
            }
            int r = rmp_core_step(mCore);
            if (r) {  // 致命エラー: result は core 内に保持
                ALOGE("session step ended r=%d", r);
                break;
            }
        }
        int result = rmp_core_stop(mCore);  // RF/KL/librmp/buffer/lock 解放、core 解放
        mCore = nullptr;
        ALOGI("session end result=%d frames=%llu acks=%llu", result,
              static_cast<unsigned long long>(mFrames), static_cast<unsigned long long>(mAcks));
        auto cb = mCb;
        if (cb) cb->onEnded(result);
    }

    static void onStateThunk(void* ctx, const char* s) {
        auto* self = static_cast<RmpProducer*>(ctx);
        ALOGI("session state %s", s);
        auto cb = self->mCb;
        if (cb) cb->onState(std::string(s));
    }
    static void onFrameThunk(void* ctx, uint32_t index, uint32_t offset, uint32_t length,
                             uint64_t pts, uint32_t flags) {
        auto* self = static_cast<RmpProducer*>(ctx);
        self->mFrames++;
        auto cb = self->mCb;
        if (cb) cb->onFrameReady(static_cast<int32_t>(index), static_cast<int32_t>(offset),
                                 static_cast<int32_t>(length), static_cast<int64_t>(pts),
                                 static_cast<int32_t>(flags));
    }

    std::mutex mLifecycle;                 // start/stop の直列化
    std::mutex mAckMu;                     // mAckQueue 保護
    std::vector<uint32_t> mAckQueue;
    std::atomic<bool> mRunning{false};
    std::thread mWorker;
    rmp_core* mCore{nullptr};
    std::shared_ptr<IRmpFrameCallback> mCb;
    ndk::ScopedAIBinder_DeathRecipient mDeath{AIBinder_DeathRecipient_new(&RmpProducer::onClientDied)};
    uint64_t mFrames{0};                   // worker スレッドのみが更新(統計ログ用)
    uint64_t mAcks{0};
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto svc = ndk::SharedRefBase::make<RmpProducer>();
    const std::string name = std::string(RmpProducer::descriptor) + "/default";
    binder_status_t st = AServiceManager_addService(svc->asBinder().get(), name.c_str());
    if (st != STATUS_OK) {
        ALOGE("addService %s failed status=%d", name.c_str(), st);
        return 1;
    }
    ALOGI("registered %s", name.c_str());
    ABinderProcess_startThreadPool();
    ABinderProcess_joinThreadPool();
    return 0;  // 到達しない
}
