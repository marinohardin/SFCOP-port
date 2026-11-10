#!/bin/sh
# Creates the draft release for TAG, or updates the one draft of that tag in place, so that it
# carries exactly the assets listed (.github/workflows/release.yml):
#
#   sh packaging/draft_release.sh TAG TITLE NOTES.md ASSETS.txt COMMIT
#
# Everything goes through the REST API by release id, because a lookup by tag does not find a
# draft. A published release of TAG is never touched: the script stops. On an existing draft
# every asset is deleted before the new ones are uploaded, so a package that was not built this
# time does not linger beside notes that say so. Needs gh, GH_TOKEN and GITHUB_REPOSITORY.
set -eu

if [ $# -ne 5 ]; then
    sed -n '5p' "$0" >&2
    exit 2
fi
tag=$1 title=$2 notes=$3 assets=$4 commit=$5
repo=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}

found=$(gh api --paginate "repos/$repo/releases?per_page=100" \
    --jq ".[] | select(.tag_name == \"$tag\") | \"\(.id) \(.draft)\"")
published=$(printf '%s\n' "$found" | awk '$2 == "false"' | head -n 1)
if [ -n "$published" ]; then
    echo "draft_release: $tag is already published; this workflow only makes drafts" >&2
    exit 1
fi
id=$(printf '%s\n' "$found" | awk '$2 == "true" { print $1; exit }')
if [ -z "$id" ]; then
    id=$(gh api -X POST "repos/$repo/releases" -f tag_name="$tag" -f target_commitish="$commit" \
        -f name="$title" -F draft=true -F prerelease=true -F body=@"$notes" --jq .id)
    echo "draft_release: created the draft $tag (id $id)"
else
    gh api -X PATCH "repos/$repo/releases/$id" -f target_commitish="$commit" -f name="$title" \
        -F body=@"$notes" --jq .id >/dev/null
    gh api --paginate "repos/$repo/releases/$id/assets?per_page=100" --jq '.[] | "\(.id) \(.name)"' |
        while read -r asset name; do
            gh api -X DELETE "repos/$repo/releases/assets/$asset"
            echo "draft_release: removed the old $name"
        done
    echo "draft_release: updated the draft $tag (id $id)"
fi
while IFS= read -r path; do
    [ -n "$path" ] || continue
    name=$(basename "$path")
    gh api -X POST "https://uploads.github.com/repos/$repo/releases/$id/assets?name=$name" \
        -H "Content-Type: application/octet-stream" --input "$path" --jq .name >/dev/null
    echo "draft_release: attached $name"
done <"$assets"
gh api "repos/$repo/releases/$id" \
    --jq '"\(.name) (draft: \(.draft))", (.assets[] | "  \(.name)  \(.size) bytes")'
