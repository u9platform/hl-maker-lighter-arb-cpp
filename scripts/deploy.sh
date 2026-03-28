#!/bin/bash
# deploy.sh — 本地 push + EC2 pull & build & restart
# 用法: ./scripts/deploy.sh [--no-restart]
set -euo pipefail

EC2="lighter-arb"
REMOTE_DIR="~/lighter-hl-arb-cpp"
SERVICE="arb-bot"

echo "=== 1. Local git status ==="
if [[ -n $(git status --porcelain) ]]; then
  echo "ERROR: Working tree not clean. Commit first."
  git status --short
  exit 1
fi

LOCAL_HASH=$(git rev-parse --short HEAD)
echo "Local HEAD: $LOCAL_HASH"

echo ""
echo "=== 2. Push to origin ==="
git push origin main

echo ""
echo "=== 3. EC2: git pull ==="
ssh $EC2 "cd $REMOTE_DIR && git pull --ff-only"

echo ""
echo "=== 4. EC2: build ==="
ssh $EC2 "cd $REMOTE_DIR/build && cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3 && make -j2 2>&1 | tail -10"
BUILD_OK=$?

if [[ $BUILD_OK -ne 0 ]]; then
  echo "ERROR: Build failed on EC2!"
  exit 1
fi

REMOTE_HASH=$(ssh $EC2 "cd $REMOTE_DIR && git rev-parse --short HEAD")
echo ""
echo "EC2 HEAD: $REMOTE_HASH"

if [[ "$LOCAL_HASH" != "$REMOTE_HASH" ]]; then
  echo "WARNING: Hash mismatch! Local=$LOCAL_HASH EC2=$REMOTE_HASH"
fi

if [[ "${1:-}" == "--no-restart" ]]; then
  echo ""
  echo "=== Skip restart (--no-restart) ==="
  echo "Done. Run manually: ssh $EC2 'sudo systemctl restart $SERVICE'"
  exit 0
fi

echo ""
echo "=== 5. EC2: restart bot ==="
ssh $EC2 "sudo systemctl restart $SERVICE"
sleep 2
ssh $EC2 "sudo systemctl status $SERVICE --no-pager -l | head -15"

echo ""
echo "=== 6. Verify: tail log ==="
sleep 3
ssh $EC2 "journalctl -u $SERVICE --no-pager -n 20"

echo ""
echo "✅ Deploy complete: $LOCAL_HASH"
