#!/usr/bin/env bash
# l01ktv_rflock_init のホストテスト。所有者は実行ユーザーに読み替え、SELinux 検査は外してビルドする。
# 使い方: host_test.sh [作業dir]   (既定: mktemp)
set -u
SELF="$(cd "$(dirname "$0")" && pwd)"
W="${1:-$(mktemp -d)}"
BIN="$W/rflock_init_host"
cc -std=c11 -Wall -Wextra -Werror -DL01KTV_RFLOCK_HOST_TEST -o "$BIN" "$SELF/l01ktv_rflock_init.c" || exit 1
pass=0; fail=0
ok() { echo "PASS $1"; pass=$((pass + 1)); }
ng() { echo "FAIL $1: $2"; fail=$((fail + 1)); }
fresh() { rm -rf "$W/d"; mkdir -m 0700 "$W/d"; }
run() { "$BIN" "$W/d" 2>"$W/log"; echo $?; }
sig() { stat -c '%i %a %h %s %Y %F' "$1"; }

# T1 初期状態: 作成され、通常ファイル 0600・単一リンク・空。
fresh; rc=$(run)
if [[ $rc == 0 && -f $W/d/rf.lock && $(stat -c '%a %h %s' "$W/d/rf.lock") == "600 1 0" ]]; then ok T1_create; else ng T1_create "rc=$rc"; fi

# T2 既存正常: inode/属性/内容/mtime を保持(truncate しない)。
fresh; printf keep > "$W/d/rf.lock"; chmod 0600 "$W/d/rf.lock"; touch -d '2020-01-01' "$W/d/rf.lock"
before=$(sig "$W/d/rf.lock"); rc=$(run); after=$(sig "$W/d/rf.lock")
if [[ $rc == 0 && $before == "$after" && $(cat "$W/d/rf.lock") == keep ]]; then ok T2_keep_existing; else ng T2_keep_existing "rc=$rc $before / $after"; fi

# T3 冪等: 2回目も成功し inode 不変。
fresh; run >/dev/null; i1=$(stat -c %i "$W/d/rf.lock"); rc=$(run); i2=$(stat -c %i "$W/d/rf.lock")
if [[ $rc == 0 && $i1 == "$i2" ]]; then ok T3_idempotent; else ng T3_idempotent "rc=$rc"; fi

# T4 稼働中(別プロセスが flock 保持): 成功し、ロックを奪わない/外さない。
fresh; run >/dev/null
flock -x "$W/d/rf.lock" sleep 3 & holder=$!; sleep 0.3
i1=$(stat -c %i "$W/d/rf.lock"); rc=$(run); i2=$(stat -c %i "$W/d/rf.lock")
if flock -n -x "$W/d/rf.lock" true; then still=no; else still=yes; fi
wait $holder
if [[ $rc == 0 && $i1 == "$i2" && $still == yes ]]; then ok T4_held_lock; else ng T4_held_lock "rc=$rc held=$still"; fi

# T5 不正 mode(0644): fail closed、mode/inode 不変。
fresh; : > "$W/d/rf.lock"; chmod 0644 "$W/d/rf.lock"; before=$(sig "$W/d/rf.lock"); rc=$(run); after=$(sig "$W/d/rf.lock")
if [[ $rc == 4 && $before == "$after" ]]; then ok T5_bad_mode; else ng T5_bad_mode "rc=$rc"; fi

# T6 symlink: fail closed、リンクも参照先も不変。
fresh; printf t > "$W/target"; chmod 0600 "$W/target"; ln -s "$W/target" "$W/d/rf.lock"
tb=$(sig "$W/target"); rc=$(run); ta=$(sig "$W/target")
if [[ $rc == 4 && -L $W/d/rf.lock && $tb == "$ta" ]]; then ok T6_symlink; else ng T6_symlink "rc=$rc"; fi
rm -f "$W/target"

# T7 dangling symlink: 参照先を作らない。
fresh; ln -s "$W/nowhere" "$W/d/rf.lock"; rc=$(run)
if [[ $rc == 4 && ! -e $W/nowhere ]]; then ok T7_dangling_symlink; else ng T7_dangling_symlink "rc=$rc"; fi

# T8 FIFO: ブロックせず fail closed。
fresh; mkfifo -m 0600 "$W/d/rf.lock"; rc=$(timeout 5 "$BIN" "$W/d" 2>/dev/null; echo $?)
if [[ $rc == 4 && -p $W/d/rf.lock ]]; then ok T8_fifo; else ng T8_fifo "rc=$rc"; fi

# T9 ディレクトリ: fail closed。
fresh; mkdir -m 0700 "$W/d/rf.lock"; rc=$(run)
if [[ $rc == 4 && -d $W/d/rf.lock ]]; then ok T9_dir; else ng T9_dir "rc=$rc"; fi

# T10 ハードリンク(nlink=2): fail closed、両方不変。
fresh; : > "$W/d/rf.lock"; chmod 0600 "$W/d/rf.lock"; ln "$W/d/rf.lock" "$W/d/alias"; rc=$(run)
if [[ $rc == 4 && $(stat -c %h "$W/d/rf.lock") == 2 ]]; then ok T10_hardlink; else ng T10_hardlink "rc=$rc"; fi

# T11 親の mode 不正(0755): 何も作らない。
fresh; chmod 0755 "$W/d"; rc=$(run)
if [[ $rc == 3 && ! -e $W/d/rf.lock ]]; then ok T11_bad_parent_mode; else ng T11_bad_parent_mode "rc=$rc"; fi

# T12 親が無い: 親も作らない。
rm -rf "$W/d"; rc=$(run)
if [[ $rc == 2 && ! -e $W/d ]]; then ok T12_no_parent; else ng T12_no_parent "rc=$rc"; fi

# T13 親が symlink: fail closed、参照先に作らない。
rm -rf "$W/d" "$W/real"; mkdir -m 0700 "$W/real"; ln -s "$W/real" "$W/d"; rc=$(run)
if [[ $rc == 2 && ! -e $W/real/rf.lock ]]; then ok T13_parent_symlink; else ng T13_parent_symlink "rc=$rc"; fi
rm -rf "$W/d" "$W/real"

# T14 umask が緩くても 0600 で作る。
fresh; rc=$( (umask 000; "$BIN" "$W/d" 2>/dev/null; echo $?) )
if [[ $rc == 0 && $(stat -c %a "$W/d/rf.lock") == 600 ]]; then ok T14_umask; else ng T14_umask "rc=$rc"; fi

echo "rflock_init host test: pass=$pass fail=$fail"
[[ $fail == 0 ]]
