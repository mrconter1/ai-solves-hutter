#!/usr/bin/env bash
# Lists cloud runs, or shows one and appends its result row to results.csv.
#
#   bench/cloud/gcp-fetch.sh            list runs with their status
#   bench/cloud/gcp-fetch.sh <run-id>   show status and log tail; when the run
#                                       is done, append its row (once) to
#                                       results/results.csv
set -euo pipefail
cd "$(dirname "$0")/../.."
project=${GCP_PROJECT:-ai-solves-hutter}
bucket=${GCP_BUCKET:-ai-solves-hutter-runs}
gcloud=${GCLOUD:-gcloud}
command -v "$gcloud" >/dev/null 2>&1 || gcloud="$LOCALAPPDATA/google-cloud-sdk/bin/gcloud.cmd"
cat_() { "$gcloud" storage cat "$1" --project "$project" 2>/dev/null; }

if [ $# -eq 0 ]; then
  for d in $("$gcloud" storage ls "gs://$bucket/runs/" --project "$project"); do
    id=$(basename "$d")
    printf '%-40s %s\n' "$id" "$(cat_ "gs://$bucket/runs/$id/STATUS" || echo '(starting)')"
  done
  exit 0
fi

id=$1
status=$(cat_ "gs://$bucket/runs/$id/STATUS" || echo '(starting)')
echo "status: $status"
cat_ "gs://$bucket/runs/$id/run.log" | tail -n 15 || true

row=$(cat_ "gs://$bucket/runs/$id/result.csv" || true)
if [ -n "$row" ]; then
  if grep -qxF "$row" results/results.csv; then
    echo "row already in results/results.csv"
  else
    echo "$row" >> results/results.csv
    echo "appended to results/results.csv:"
    echo "$row"
  fi
fi
