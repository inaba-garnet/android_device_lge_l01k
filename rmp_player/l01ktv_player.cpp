// SPDX-License-Identifier: MIT
// l01ktv_player: PROTECTED_ES 経路の system側 bridge（製品版）。
// 由来: research の surface_probe.cpp（secure Surface＋secure MediaCodec＋ACK駆動consume、実機表示実績）を
//   製品化。producer は vendor AIDL HAL(vendor.l01k.tv.rmp / IRmpProducer) を client として駆動する(S-c)。
//   旧版の posix_spawn(rf_secure)+socketpair(frame_ipc) は廃止(#A境界を hal_attribute 経由 binder へ正規化)。
// 不変の境界: 保護バッファ payload を CPU で map/read しない。鍵・librmp中身・TAには触れない。
//   FLAG_SECURE/secure codec/protected Surface のまま扱い、clear 取り出し経路を持たせない。
// ビルド: ROMツリー(Soong, libgui/libmedia… + libbinder_ndk + vendor.l01k.tv.rmp-V1-ndk)。standalone 不可。
// 初版は自前で protected Surface を作る（surface_probe と同方式）。APK からの Surface 受け渡し(AIDL)は次段。
// ログ: 各イベント行を stdout と logcat(tag l01ktv_player) の両方へ出す。payload・鍵・ECM は出さない。
#define LOG_TAG "l01ktv_player"
#ifdef __LP64__
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#endif
#include <binder/ProcessState.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <media/MediaCodecBuffer.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/NativeHandle.h>
#include <ui/DisplayState.h>
#include "SecureBuffer.h"

#include <aidl/vendor/l01k/tv/rmp/IRmpProducer.h>
#include <aidl/vendor/l01k/tv/rmp/BnRmpFrameCallback.h>
#include <aidl/vendor/l01k/tv/rmp/RmpTuneParams.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <time.h>
#include <unistd.h>

using namespace android;
using aidl::vendor::l01k::tv::rmp::BnRmpFrameCallback;
using aidl::vendor::l01k::tv::rmp::IRmpFrameCallback;
using aidl::vendor::l01k::tv::rmp::IRmpProducer;
using aidl::vendor::l01k::tv::rmp::RmpTuneParams;

// 1イベント1行。stdout(試験時の直接実行)と logcat(init 起動時)へ同じ文面を出す。
__attribute__((format(printf, 1, 2))) static void report(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
}

static volatile sig_atomic_t stopped;
static void interrupted(int) { stopped = 1; }
static double mono() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

struct Tune { int channel; int station; int ecmPid; int videoPid; unsigned seconds; };

// HAL からの保護ES通知(onFrameReady)の内容。payload は運ばない(index/範囲/PTS のみ)。
struct FrameInfo { int32_t index, offset, length; int64_t pts; int32_t flags; };

// vendor HAL → player のコールバック。binder スレッドで呼ばれるのでキュー化し、feed ループが取り出す。
class PlayerCb : public BnRmpFrameCallback {
  public:
    std::mutex mu;
    std::deque<FrameInfo> frames;
    std::atomic<bool> ended{false};
    std::atomic<int> result{0};

    ndk::ScopedAStatus onState(const std::string& s) override {
        report("PRODUCER_STATE %s\n", s.c_str());
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onFrameReady(int32_t index, int32_t offset, int32_t length,
                                    int64_t pts, int32_t flags) override {
        std::lock_guard<std::mutex> lk(mu);
        frames.push_back(FrameInfo{index, offset, length, pts, flags});
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onEnded(int32_t r) override {
        result.store(r); ended.store(true);
        report("PRODUCER_ENDED result=%d\n", r);
        return ndk::ScopedAStatus::ok();
    }
};

static int feed(const sp<MediaCodec>& codec, const Tune& t) {
    Vector<sp<MediaCodecBuffer>> buffers;
    status_t rc = codec->getInputBuffers(&buffers);
    report("INPUT_BUFFERS rc=%d count=%zu\n", rc, buffers.size());
    if (rc != OK || buffers.size()<2 || buffers.size()>32) return 4;
    unsigned count=static_cast<unsigned>(buffers.size());
    std::vector<int> fds(count);std::vector<bool> owned(count,false),queued(count,false);
    for (unsigned i=0; i<count; i++) {
        if (buffers[i]->capacity() != 1568768) { report("UNSUPPORTED_CAPACITY %zu\n",buffers[i]->capacity()); return 5; }
        // secure(.secure)選択時、client/codec バッファは SecureBuffer 自体で、通常の mappable base は持たない。
        if (buffers[i]->base() != nullptr) { report("INPUT_IS_NOT_OPAQUE\n"); return 5; }
        auto* secure = static_cast<SecureBuffer*>(buffers[i].get());
        if (secure->getDestinationType() != ICrypto::kDestinationTypeNativeHandle) return 5;
        auto* handle = static_cast<native_handle_t*>(secure->getDestinationPointer());
        if (!handle || handle->numFds != 1) return 5;
        fds[i] = handle->data[0];
    }
    double deadline=mono()+3; unsigned available=0;
    while (available<count && mono()<deadline) {
        size_t index; rc=codec->dequeueInputBuffer(&index,100000);
        if (rc==-EAGAIN) continue;
        if (rc!=OK || index>=count || owned[index]) return 6;
        owned[index]=true; available++;
    }
    if (available!=count) return 6;

    // vendor HAL(producer) へ接続。保護バッファFDは ParcelFileDescriptor[] で渡す(binderがdupする)。
    const std::string svcName = std::string(IRmpProducer::descriptor) + "/default";
    ndk::SpAIBinder binder(AServiceManager_waitForService(svcName.c_str()));
    std::shared_ptr<IRmpProducer> producer = IRmpProducer::fromBinder(binder);
    if (!producer) { report("HAL_NOT_FOUND %s\n", svcName.c_str()); return 7; }
    std::vector<ndk::ScopedFileDescriptor> secureBuffers; secureBuffers.reserve(count);
    for (unsigned i=0;i<count;i++){ int d=dup(fds[i]); if(d<0){report("DUP_FAIL\n");return 7;} secureBuffers.emplace_back(d); }
    auto cb = ndk::SharedRefBase::make<PlayerCb>();
    RmpTuneParams params; params.channel=t.channel; params.station=t.station;
    params.ecmPid=t.ecmPid; params.videoPid=t.videoPid; params.bufferSize=1568768;
    int64_t sessionId=0;
    ndk::ScopedAStatus st = producer->start(params, secureBuffers, cb, &sessionId);
    if(!st.isOk()){ report("HAL_START_FAIL ex=%d svc=%d\n", st.getExceptionCode(), st.getServiceSpecificError()); return 7; }
    report("PRODUCER session=%lld\n",(long long)sessionId);

    uint64_t input=0,output=0,acks=0; bool failed=false;
    double start=mono(),nextLog=start;
    while(!stopped && mono()-start<t.seconds+5 && !failed && !cb->ended.load()){
        for(int i=0;i<16;i++){ // codec が返した入力スロット → HAL へ ACK（REUSE許可）
            size_t index=0;rc=codec->dequeueInputBuffer(&index,0);
            if(rc==-EAGAIN)break;
            if(rc!=OK||index>=count||owned[index]||!queued[index]){report("INPUT_RETURN_ERROR rc=%d\n",rc);failed=true;break;}
            ndk::ScopedAStatus a=producer->ack(sessionId,static_cast<int32_t>(index));
            if(!a.isOk()){report("ACK_FAIL ex=%d\n",a.getExceptionCode());failed=true;break;}
            owned[index]=true;queued[index]=false;acks++;
        }
        for(int i=0;i<16&&!failed;i++){ // HAL からの保護ES通知 → codec へ投入
            FrameInfo fr; bool have=false;
            { std::lock_guard<std::mutex> lk(cb->mu); if(!cb->frames.empty()){fr=cb->frames.front();cb->frames.pop_front();have=true;} }
            if(!have)break;
            if(fr.index<0||static_cast<unsigned>(fr.index)>=count||!owned[fr.index]||
                (uint64_t)fr.offset+fr.length>buffers[fr.index]->capacity()){report("BAD_FRAME_NOTIFY\n");failed=true;break;}
            int64_t ptsUs=(fr.flags&1)?fr.pts*1000000LL/90000:input*33367;
            rc=codec->queueInputBuffer(fr.index,fr.offset,fr.length,ptsUs,0);
            if(rc!=OK){report("QUEUE_ERROR rc=%d\n",rc);failed=true;break;}
            owned[fr.index]=false;queued[fr.index]=true;input++;
        }
        for(int i=0;i<16&&!failed;i++){ // 出力 → protected Surface へ描画
            size_t index,offset,size;int64_t pts;uint32_t flags;
            rc=codec->dequeueOutputBuffer(&index,&offset,&size,&pts,&flags,0);
            if(rc==-EAGAIN)break;
            if(rc==INFO_FORMAT_CHANGED){sp<AMessage> fmt;codec->getOutputFormat(&fmt);report("OUTPUT_FORMAT %s\n",fmt?fmt->debugString().c_str():"none");continue;}
            if(rc==INFO_OUTPUT_BUFFERS_CHANGED)continue;
            if(rc!=OK){report("OUTPUT_ERROR rc=%d\n",rc);failed=true;break;}
            rc=codec->renderOutputBufferAndRelease(index);
            if(rc!=OK){report("RENDER_ERROR rc=%d\n",rc);failed=true;break;}
            output++;
        }
        if(mono()>=nextLog){report("PLAYER_PROGRESS t=%.1f input=%llu output=%llu ack=%llu\n",mono()-start,(unsigned long long)input,(unsigned long long)output,(unsigned long long)acks);nextLog=mono()+1;}
        usleep(2000);
    }
    producer->stop(sessionId);   // RF/KL/librmp/buffer 解放完了まで戻らない(冪等)
    int producerResult=cb->result.load();
    report("PLAYER_SUMMARY input=%llu output=%llu acks=%llu producer_result=%d failed=%d\n",
           (unsigned long long)input,(unsigned long long)output,(unsigned long long)acks,producerResult,failed);
    return (!failed && output>0 && producerResult==0) ? 0 : 8;
}

static int envInt(const char* k,int def,int lo,int hi){const char* v=getenv(k);if(!v)return def;char* e;long n=strtol(v,&e,0);return(!*e&&n>=lo&&n<=hi)?(int)n:def;}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);signal(SIGTERM,interrupted);signal(SIGINT,interrupted);
    // 初版: 選局情報は env（本番は APK/PSI から。station/ecm/video は L01KTV_BACKEND_CONTRACT の選局後値）。
    Tune t{ envInt("L01KTV_CHANNEL",15,13,62), envInt("L01KTV_STATION",0x7fd3,1,0xffff),
            envInt("L01KTV_ECM_PID",0x61,0,8190), envInt("L01KTV_VIDEO_PID",0x111,0,8190),
            (unsigned)envInt("L01KTV_PLAY_SECONDS",20,1,60) };
    // libbinder と libbinder_ndk は ProcessState を共有する。startThreadPool() が起動する binder
    // スレッドで ndk 登録 binder(HAL callback)への受信も配信される。ndk 側で別途 threadpool を
    // 設定/縮小すると "cannot be shrunk after starting" で abort するため呼ばない。
    ProcessState::self()->startThreadPool();
    sp<ALooper> looper=new ALooper;looper->setName("l01ktv-player");looper->start();
    sp<SurfaceComposerClient> composer=new SurfaceComposerClient;
    if(composer->initCheck()!=OK){report("PLAYER_EXIT result=2 what=composer\n");return 2;}
    auto control=composer->createSurface(String8("L01kTv-Protected"),1920,1080,PIXEL_FORMAT_RGBA_8888,gui::ISurfaceComposerClient::eSecure);
    if(!control||!control->isValid()){report("PLAYER_EXIT result=2 what=surface\n");return 2;}
    const auto displays=SurfaceComposerClient::getPhysicalDisplayIds();
    if(displays.empty()){report("PLAYER_EXIT result=2 what=display\n");return 2;}
    ui::DisplayState ds;
    status_t rc=SurfaceComposerClient::getDisplayState(SurfaceComposerClient::getPhysicalDisplayToken(displays.front()),&ds);
    if(rc!=OK||ds.layerStackSpaceRect.getWidth()<=0){report("PLAYER_EXIT result=2 what=display_state\n");return 2;}
    const int width=ds.layerStackSpaceRect.getWidth(); const int height=width*9/16;
    rc=SurfaceComposerClient::Transaction().setLayer(control,0x7ffffff0).setLayerStack(control,ds.layerStack)
        .setMatrix(control,width/1920.0f,0,0,height/1080.0f).show(control).apply();
    report("SURFACE_CREATE rc=%d display=%dx%d\n",rc,width,height);
    sp<Surface> surface=control->getSurface();
    sp<MediaCodec> codec=MediaCodec::CreateByComponentName(looper,AString("OMX.qcom.video.decoder.mpeg2.secure"),&rc);
    report("CODEC_CREATE rc=%d\n",rc);
    int result=3;
    if(codec&&rc==OK){
        sp<AMessage> format=new AMessage;format->setString("mime","video/mpeg2");format->setInt32("width",1920);format->setInt32("height",1080);format->setInt32("max-input-size",1568768);format->setInt32("protected",1);format->setInt32("android._num-input-buffers",32);
        rc=codec->configure(format,surface,nullptr,0);report("CODEC_CONFIGURE rc=%d\n",rc);
        if(rc==OK){rc=codec->start();report("CODEC_START rc=%d\n",rc);if(rc==OK){result=feed(codec,t);report("CODEC_STOP rc=%d\n",codec->stop());}}
        report("CODEC_RELEASE rc=%d\n",codec->release());
    }
    codec.clear();
    SurfaceComposerClient::Transaction().hide(control).apply();surface.clear();control.clear();composer->dispose();composer.clear();looper->stop();looper.clear();
    report("PLAYER_EXIT result=%d\n",result);
    return result;
}
