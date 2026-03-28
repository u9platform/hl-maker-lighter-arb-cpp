#!/bin/bash
# Sync live data from production server to local repo
# Codex can read these files without SSH access
set -e

SSH_KEY="/Users/aibot01/.ssh/lighter-arb-bot.pem"
SSH_HOST="ubuntu@35.78.180.141"
LOCAL_DIR="/Users/aibot01/clawd/PROJECTS/lighter-hl-arb/live"
REMOTE_DIR="/home/ubuntu/lighter-hl-arb-cpp"

mkdir -p "$LOCAL_DIR"

# 1. Trade journal CSV (append-only, full history)
scp -o ConnectTimeout=5 -i "$SSH_KEY" "$SSH_HOST:$REMOTE_DIR/trades.csv" "$LOCAL_DIR/trades.csv" 2>/dev/null || true

# 2. Recent log (last 500 lines to keep file small)
ssh -o ConnectTimeout=5 -i "$SSH_KEY" "$SSH_HOST" "tail -500 /tmp/arb_live.log" > "$LOCAL_DIR/arb_live_tail.log" 2>/dev/null || true

# 3. Performance telemetry (grep perf lines)
ssh -o ConnectTimeout=5 -i "$SSH_KEY" "$SSH_HOST" "grep 'perf ' /tmp/arb_live.log | tail -200" > "$LOCAL_DIR/perf_trace.log" 2>/dev/null || true

# 4. Current process info
ssh -o ConnectTimeout=5 -i "$SSH_KEY" "$SSH_HOST" "ps aux | grep arb_live | grep -v grep; echo '---'; tail -1 /tmp/arb_live.log | grep telem || tail -5 /tmp/arb_live.log" > "$LOCAL_DIR/status.txt" 2>/dev/null || true

# 5. Current .env config (redact secrets)
ssh -o ConnectTimeout=5 -i "$SSH_KEY" "$SSH_HOST" "cd $REMOTE_DIR && grep -E '^(PAIR_SIZE|SPREAD|MAX_POS|DRY_RUN|HL_COIN|LIGHTER_MARKET)' .env" > "$LOCAL_DIR/config.txt" 2>/dev/null || true

# Timestamp
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$LOCAL_DIR/.last_sync"

echo "✅ Synced at $(date -u '+%H:%M:%S UTC')"
echo "  trades.csv: $(wc -l < "$LOCAL_DIR/trades.csv" 2>/dev/null || echo 0) lines"
echo "  arb_live_tail.log: $(wc -l < "$LOCAL_DIR/arb_live_tail.log" 2>/dev/null || echo 0) lines"
echo "  perf_trace.log: $(wc -l < "$LOCAL_DIR/perf_trace.log" 2>/dev/null || echo 0) lines"
