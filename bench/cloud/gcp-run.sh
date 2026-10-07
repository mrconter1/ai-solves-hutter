#!/usr/bin/env bash
# Starts one benchmark run on a Google Cloud VM and returns immediately.
#
#   bench/cloud/gcp-run.sh <attempt> [input=enwik9] [machine=n2d-standard-4]
#
# The VM gets the committed tree (git archive of HEAD), runs bench/run.sh under
# the same limits as locally, uploads the result row and logs to the bucket and
# deletes itself. --max-run-duration deletes it anyway if anything hangs, so it
# can never keep billing. Collect results with bench/cloud/gcp-fetch.sh.
#
# Settings (env): GCP_PROJECT, GCP_ZONE, GCP_BUCKET, MAX_HOURS, MAX_VMS (default 2),
# NOTE, and CFLAGS (extra build flags, e.g. CFLAGS="-DTABLE_BITS=26" for a
# release-size run).
set -euo pipefail
cd "$(dirname "$0")/../.."

attempt=${1:?usage: gcp-run.sh <attempt> [input] [machine]}
input=${2:-enwik9}
machine=${3:-n2d-standard-4}
project=${GCP_PROJECT:-ai-solves-hutter}
zone=${GCP_ZONE:-europe-north1-a}
bucket=${GCP_BUCKET:-ai-solves-hutter-runs}
note=${NOTE:-gcp $machine}
cflags=${CFLAGS:-}
case "$cflags$note" in *,*) echo "NOTE and CFLAGS must not contain commas" >&2; exit 1;; esac
gcloud=${GCLOUD:-gcloud}
command -v "$gcloud" >/dev/null 2>&1 || gcloud="$LOCALAPPDATA/google-cloud-sdk/bin/gcloud.cmd"

# A run must be reproducible from a commit: refuse uncommitted changes.
if [ -n "$(git status --porcelain -- "attempts/$attempt" bench)" ]; then
  echo "attempts/$attempt or bench/ has uncommitted changes; commit first" >&2
  exit 1
fi
commit=$(git rev-parse --short HEAD)

# Default limit: both directions at the harness time limit, plus 2 h of setup.
. bench/limits.sh
max_hours=${MAX_HOURS:-$(awk -v h="$TIME_LIMIT_H" -v f="$SPEED_FACTOR" 'BEGIN{printf "%d", 2*h/f + 2}')}

run_id="$(date -u +%Y%m%d-%H%M%S)-$attempt-$input"
vm="hutter-$(echo "$run_id" | tr 'A-Z_' 'a-z-' | cut -c1-55)"

# Cost guard: never more than MAX_VMS benchmark VMs at once.
max_vms=${MAX_VMS:-2}
running=$("$gcloud" compute instances list --project "$project" --filter="name:hutter-*" --format="value(name)" 2>/dev/null | grep -c . || true)
if [ "$running" -ge "$max_vms" ]; then
  echo "$running benchmark VMs already running (MAX_VMS=$max_vms); wait for one to finish" >&2
  exit 2
fi

"$gcloud" storage buckets describe "gs://$bucket" --project "$project" >/dev/null 2>&1 ||
  "$gcloud" storage buckets create "gs://$bucket" --project "$project" --location "${zone%-*}" --uniform-bucket-level-access

tmp=$(mktemp -d)
git archive --format=tar.gz -o "$tmp/repo.tar.gz" HEAD
"$gcloud" storage cp "$tmp/repo.tar.gz" "gs://$bucket/runs/$run_id/repo.tar.gz" --project "$project" >/dev/null
batch=no
if [ -n "${BATCH_VARIANTS:-}" ]; then
  "$gcloud" storage cp "$BATCH_VARIANTS" "gs://$bucket/runs/$run_id/variants.txt" --project "$project" >/dev/null
  batch=yes
fi
rm -rf "$tmp"

"$gcloud" compute instances create "$vm" \
  --project "$project" --zone "$zone" --machine-type "$machine" \
  --image-family debian-12 --image-project debian-cloud \
  --boot-disk-size 50GB --boot-disk-type pd-balanced \
  --scopes cloud-platform \
  --max-run-duration "${max_hours}h" --instance-termination-action DELETE \
  --metadata "bucket=$bucket,run-id=$run_id,attempt=$attempt,input=$input,commit=$commit,note=$note,batch=$batch,cflags=$cflags" \
  --metadata-from-file startup-script=bench/cloud/vm-startup.sh \
  --format "value(name)"

echo "started $vm ($machine, $zone), deletes itself within ${max_hours} h"
echo "run id: $run_id"
echo "check:  bench/cloud/gcp-fetch.sh $run_id"
