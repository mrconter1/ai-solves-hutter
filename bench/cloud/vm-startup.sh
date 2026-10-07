#!/usr/bin/env bash
# Runs on the cloud VM at boot (as root), via the startup-script metadata set by
# gcp-run.sh. Fetches the exported repo and the data, runs bench/run.sh exactly
# as it runs locally, uploads the results, then deletes the VM.
#
# Every input comes from instance metadata, so this script holds no secrets.
set -uo pipefail
md() { curl -fs -H 'Metadata-Flavor: Google' "http://metadata.google.internal/computeMetadata/v1/instance/attributes/$1"; }
BUCKET=$(md bucket)
RUN_ID=$(md run-id)
ATTEMPT=$(md attempt)
INPUT=$(md input)
COMMIT=$(md commit)
NOTE=$(md note || true)
ZONE=$(curl -fs -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/zone | awk -F/ '{print $NF}')
OUT="gs://$BUCKET/runs/$RUN_ID"
LOG=/var/log/hutter-run.log

finish() {
  gcloud storage cp "$LOG" "$OUT/run.log" >/dev/null 2>&1 || true
  echo "$1" | gcloud storage cp - "$OUT/STATUS" >/dev/null 2>&1 || true
  # Delete ourselves; --max-run-duration on the VM is the backstop if this fails.
  gcloud compute instances delete "$(hostname)" --zone "$ZONE" --quiet >/dev/null 2>&1 || shutdown -h now
}
trap 'finish "FAILED: startup script error at line $LINENO"' ERR

exec > >(tee -a "$LOG") 2>&1
echo "== $(date -u +%FT%TZ) run $RUN_ID: $ATTEMPT on $INPUT at $COMMIT"
echo "running" | gcloud storage cp - "$OUT/STATUS"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential musl-tools time python3 curl >/dev/null

mkdir -p /work/repo && cd /work/repo
gcloud storage cp "$OUT/repo.tar.gz" /work/repo.tar.gz
tar -xzf /work/repo.tar.gz

# Data: reuse the bucket's copy of enwik9.zip, or fetch it once and keep it.
mkdir -p data
if ! gcloud storage cp "gs://$BUCKET/data/enwik9.zip" data/enwik9.zip 2>/dev/null; then
  curl -fL --retry 3 -o data/enwik9.zip http://mattmahoney.net/dc/enwik9.zip
  gcloud storage cp data/enwik9.zip "gs://$BUCKET/data/enwik9.zip"
fi
bash bench/fetch.sh

rc=0
COMMIT="$COMMIT" NOTE="$NOTE" bash bench/run.sh "$ATTEMPT" "$INPUT" || rc=$?

tail -n 1 results/results.csv | gcloud storage cp - "$OUT/result.csv"
gcloud storage cp work/"$ATTEMPT"/*.time "$OUT/" 2>/dev/null || true
trap - ERR
if [ "$rc" -eq 0 ]; then finish "OK"; else finish "FAILED: run.sh exit $rc"; fi
