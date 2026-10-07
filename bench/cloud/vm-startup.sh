#!/usr/bin/env bash
# Google Cloud adapter, VM side. Runs at boot (as root) via the startup-script
# metadata set by gcp-run.sh: fetches the exported tree and the cached data
# from the bucket, hands over to the provider-neutral bench/remote-run.sh,
# uploads its output and deletes the VM.
#
# Every input comes from instance metadata, so this script holds no secrets.
set -uo pipefail
md() { curl -fs -H 'Metadata-Flavor: Google' "http://metadata.google.internal/computeMetadata/v1/instance/$1"; }
BUCKET=$(md attributes/bucket)
RUN_ID=$(md attributes/run-id)
ATTEMPT=$(md attributes/attempt)
INPUT=$(md attributes/input)
export COMMIT=$(md attributes/commit)
export NOTE=$(md attributes/note || true)
export CFLAGS=$(md attributes/cflags || true)
ZONE=$(md zone | awk -F/ '{print $NF}')
RUN="gs://$BUCKET/runs/$RUN_ID"

upload() { gcloud storage cp /work/out/* "$RUN/" >/dev/null 2>&1 || true; }
bye() {
  # Delete ourselves; --max-run-duration on the VM is the backstop if this fails.
  gcloud compute instances delete "$(hostname)" --zone "$ZONE" --quiet >/dev/null 2>&1 || shutdown -h now
}

mkdir -p /work/repo /work/out
echo "running" > /work/out/STATUS; upload
cd /work/repo
if ! gcloud storage cp "$RUN/repo.tar.gz" /work/repo.tar.gz || ! tar -xzf /work/repo.tar.gz; then
  echo "FAILED: could not fetch the code" > /work/out/STATUS; upload; bye; exit 1
fi

# Data cache: reuse the bucket's enwik9.zip, or let fetch.sh download it once
# and keep a copy for the next run.
mkdir -p data
cached=yes
gcloud storage cp "gs://$BUCKET/data/enwik9.zip" data/enwik9.zip >/dev/null 2>&1 || cached=no

if [ "$(md attributes/batch || true)" = yes ] && gcloud storage cp "$RUN/variants.txt" /work/variants.txt >/dev/null 2>&1; then
  export BATCH_VARIANTS=/work/variants.txt
fi
bash bench/remote-run.sh "$ATTEMPT" "$INPUT" /work/out
[ "$cached" = no ] && [ -f data/enwik9.zip ] && gcloud storage cp data/enwik9.zip "gs://$BUCKET/data/enwik9.zip" >/dev/null 2>&1
upload
bye
