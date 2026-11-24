#!/bin/bash
# SSH protocol test on Linux (runs inside the build container): the SSHD
# protocol code (net/ssh, built with plat_host.c) against OpenSSH's ssh,
# sftp and scp. Exec jobs run with /bin/sh; SFTP's /C is a scratch dir.
#
# Usage: tests/ssh/host.sh

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
PORT=2222
trap 'kill $SSHD 2>/dev/null; rm -rf "$WORK"' EXIT

make -s -C "$ROOT/net/ssh" host
mkdir -p "$WORK/srv" "$WORK/root" "$WORK/home"
cp "$ROOT/net/ssh/build/sshd-host" "$WORK/srv/sshd"
ssh-keygen -q -t ed25519 -N "" -C test@host -f "$WORK/home/id"
ssh-keygen -q -t ed25519 -N "" -C other -f "$WORK/home/other"
{ echo "# authorized keys"; cat "$WORK/home/id.pub"; } > "$WORK/srv/AUTHKEYS.TXT"
printf 'correct horse\ncorrect horse\n' | "$WORK/srv/sshd" /PASSWD > /dev/null

SSHD_ROOT="$WORK/root" "$WORK/srv/sshd" /P $PORT < /dev/null 2> "$WORK/sshd.log" &
SSHD=$!
sleep 1

OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$WORK/home/known" -o LogLevel=ERROR
      -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=10)
fail=0
check() {   # check "what" command...
  local what=$1; shift
  if "$@" > "$WORK/out" 2>&1; then echo "  ok   $what"; else
    echo "  FAIL $what"; sed 's/^/       /' "$WORK/out" | head -20; fail=1; fi
}

echo "== exec"
check "exec: echo" bash -c "ssh ${OPTS[*]} -i $WORK/home/id -p $PORT dos@127.0.0.1 'echo hello world' | grep -qx 'hello world'"
check "exec: exit status" bash -c "ssh ${OPTS[*]} -i $WORK/home/id -p $PORT dos@127.0.0.1 'exit 3'; test \$? = 3"
check "exec: 1 MB of output" bash -c "test \$(ssh ${OPTS[*]} -i $WORK/home/id -p $PORT dos@127.0.0.1 'head -c 1048576 /dev/zero' | wc -c) = 1048576"

echo "== authentication"
check "unknown key refused" bash -c "! ssh ${OPTS[*]} -i $WORK/home/other -p $PORT dos@127.0.0.1 true"
cat > "$WORK/askpass" <<'EOF'
#!/bin/sh
echo "$PASS"
EOF
chmod +x "$WORK/askpass"
pw_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=$WORK/home/known -o LogLevel=ERROR -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1"
check "password login" bash -c "PASS='correct horse' SSH_ASKPASS=$WORK/askpass SSH_ASKPASS_REQUIRE=force DISPLAY=x setsid ssh $pw_opts -p $PORT dos@127.0.0.1 'echo pw-ok' < /dev/null | grep -qx pw-ok"
check "wrong password refused" bash -c "! PASS='wrong horse' SSH_ASKPASS=$WORK/askpass SSH_ASKPASS_REQUIRE=force DISPLAY=x setsid ssh $pw_opts -p $PORT dos@127.0.0.1 true < /dev/null"
check "host key fingerprint matches the log" bash -c "grep -q \"\$(ssh-keygen -l -F '[127.0.0.1]:$PORT' -f $WORK/home/known | awk '/ED25519/ {print \$3}')\" $WORK/sshd.log"

echo "== sftp / scp"
head -c 300000 /dev/urandom > "$WORK/home/blob"
cat > "$WORK/batch" <<EOF
mkdir /C/DIR
put $WORK/home/blob /C/DIR/BLOB.BIN
ls -l /C/DIR
rename /C/DIR/BLOB.BIN /C/DIR/B2.BIN
get /C/DIR/B2.BIN $WORK/home/back
ls /
EOF
check "sftp batch" sftp -b "$WORK/batch" "${OPTS[@]}" -i "$WORK/home/id" -P $PORT dos@127.0.0.1
check "sftp: file came back unchanged" cmp "$WORK/home/blob" "$WORK/home/back"
check "sftp: stored on the server" cmp "$WORK/home/blob" "$WORK/root/DIR/B2.BIN"
check "scp upload" scp -q "${OPTS[@]}" -i "$WORK/home/id" -P $PORT "$WORK/home/blob" dos@127.0.0.1:/C/SCP.BIN
check "scp: stored" cmp "$WORK/home/blob" "$WORK/root/SCP.BIN"
check "scp download" scp -q "${OPTS[@]}" -i "$WORK/home/id" -P $PORT dos@127.0.0.1:/C/SCP.BIN "$WORK/home/scp2"
check "scp: downloaded" cmp "$WORK/home/blob" "$WORK/home/scp2"
cat > "$WORK/batch2" <<EOF
rm /C/DIR/B2.BIN
rmdir /C/DIR
EOF
check "sftp remove" sftp -b "$WORK/batch2" "${OPTS[@]}" -i "$WORK/home/id" -P $PORT dos@127.0.0.1
check "sftp: removed" test ! -e "$WORK/root/DIR"

echo "== rekey"
check "1 MB with rekeying every 64 KB" bash -c "test \$(ssh ${OPTS[*]} -o RekeyLimit=64K -i $WORK/home/id -p $PORT dos@127.0.0.1 'head -c 1048576 /dev/zero' | wc -c) = 1048576"

kill $SSHD 2>/dev/null || true
echo "-- server log"
sed 's/^/   /' "$WORK/sshd.log" | tail -25
if [ $fail = 0 ]; then echo "SSH HOST TEST: success"; else echo "SSH HOST TEST: failure"; exit 1; fi
