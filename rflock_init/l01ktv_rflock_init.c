/* SPDX-License-Identifier: MIT
 * l01ktv_rflock_init: post-fs-data で /data/vendor/l01ktv/rf.lock を「無ければ作る」だけの一回実行ツール。
 * RMP HAL と clear backend が flock で RF を排他する共有ロックの初期配置(/data 初期化直後も RMP が動くように)。
 *
 * 不変条件:
 *  - 既存の rf.lock は一切変更しない(unlink/rename/truncate/chmod/chown をしない。inode を維持)。
 *  - 新規作成は O_CREAT|O_EXCL|O_NOFOLLOW、root:root 0600。親ディレクトリは作らない(init rc の担当)。
 *  - 親が root:root 0700 の実ディレクトリでない、既存が root:root 0600 の単一リンク通常ファイルでない、
 *    SELinux 型が l01ktv_state_file でない、のいずれかなら何も直さず非0終了(fail closed)。
 *  - flock は取らない(稼働中の排他契約に干渉しない)。
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef L01KTV_RFLOCK_HOST_TEST
/* ホストテスト: 所有者は実行ユーザー、ディレクトリは引数、SELinux 検査なし、ログは stderr。 */
#define OWNER_UID geteuid()
#define OWNER_GID getegid()
#define LOGI(...) do { fprintf(stderr, "I " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "E " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define LOG_TAG "l01ktv_rflock"
#include <log/log.h>
#include <selinux/selinux.h>
#define OWNER_UID 0
#define OWNER_GID 0
#define LOGI(...) ALOGI(__VA_ARGS__)
#define LOGE(...) ALOGE(__VA_ARGS__)
#endif

#define LOCK_DIR "/data/vendor/l01ktv"
#define LOCK_NAME "rf.lock"
#define STATE_TYPE ":l01ktv_state_file:"

enum { RC_OK = 0, RC_NO_DIR = 2, RC_BAD_DIR = 3, RC_BAD_LOCK = 4, RC_BAD_LABEL = 5, RC_IO = 6 };

static int label_ok(int fd, const char *what) {
#ifdef L01KTV_RFLOCK_HOST_TEST
	(void)fd; (void)what;
	return 1;
#else
	char *con = NULL;
	if (fgetfilecon(fd, &con) < 0) { LOGE("%s: getfilecon errno=%d", what, errno); return 0; }
	int ok = strstr(con, STATE_TYPE) != NULL;
	if (!ok) LOGE("%s: unexpected label %s", what, con);
	freecon(con);
	return ok;
#endif
}

static int lock_attrs_ok(const struct stat *st) {
	return S_ISREG(st->st_mode) && st->st_uid == OWNER_UID && st->st_gid == OWNER_GID &&
	       (st->st_mode & 07777) == 0600 && st->st_nlink == 1;
}

int main(int argc, char **argv) {
	const char *dir = LOCK_DIR;
#ifdef L01KTV_RFLOCK_HOST_TEST
	if (argc > 1) dir = argv[1];
#else
	(void)argc; (void)argv;
#endif
	umask(077);

	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dfd < 0) { LOGE("lock dir unavailable errno=%d", errno); return RC_NO_DIR; }
	struct stat st;
	if (fstat(dfd, &st) < 0) { LOGE("lock dir fstat errno=%d", errno); close(dfd); return RC_IO; }
	if (!S_ISDIR(st.st_mode) || st.st_uid != OWNER_UID || st.st_gid != OWNER_GID || (st.st_mode & 07777) != 0700) {
		LOGE("lock dir attrs rejected uid=%u gid=%u mode=%o", (unsigned)st.st_uid, (unsigned)st.st_gid,
		     (unsigned)(st.st_mode & 07777));
		close(dfd); return RC_BAD_DIR;
	}
	if (!label_ok(dfd, "lock dir")) { close(dfd); return RC_BAD_LABEL; }

	int created = 1;
	int fd = openat(dfd, LOCK_NAME, O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0 && errno == EEXIST) {
		created = 0;
		/* O_NONBLOCK: FIFO 等が置かれていても待たない。O_NOFOLLOW: symlink は ELOOP で拒否。 */
		fd = openat(dfd, LOCK_NAME, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0) {
		int e = errno;
		LOGE("rf.lock open rejected errno=%d", e);
		close(dfd);
		return e == ELOOP ? RC_BAD_LOCK : RC_IO;
	}
	if (fstat(fd, &st) < 0) { LOGE("rf.lock fstat errno=%d", errno); close(fd); close(dfd); return RC_IO; }
	if (!lock_attrs_ok(&st)) {
		LOGE("rf.lock attrs rejected type=%o uid=%u gid=%u mode=%o nlink=%u", (unsigned)(st.st_mode & S_IFMT),
		     (unsigned)st.st_uid, (unsigned)st.st_gid, (unsigned)(st.st_mode & 07777), (unsigned)st.st_nlink);
		close(fd); close(dfd); return RC_BAD_LOCK;
	}
	if (!label_ok(fd, "rf.lock")) { close(fd); close(dfd); return RC_BAD_LABEL; }

	LOGI("rf.lock %s", created ? "created" : "present");
	close(fd);
	close(dfd);
	return RC_OK;
}
