#!/usr/bin/env bash
set -euo pipefail

copy_private_secret() {
    local source="$1"
    local destination="$2"

    if [[ ! -f "$source" || -L "$source" ]]; then
        echo "required PostgreSQL credential file is unavailable" >&2
        exit 1
    fi
    install -o postgres -g postgres -m 0600 "$source" "$destination"
}

# Docker bind mounts retain host ownership. Copying while this wrapper still runs
# as root preserves strict host modes while making the temporary container copy
# readable after the standard entrypoint switches to the postgres account.
install -d -o postgres -g postgres -m 0700 /run/shin-secrets
copy_private_secret "$POSTGRES_PASSWORD_FILE" /run/shin-secrets/postgres_migrator_password
copy_private_secret "$SHIN_RUNTIME_PASSWORD_FILE" /run/shin-secrets/shin_runtime_password

export POSTGRES_PASSWORD_FILE=/run/shin-secrets/postgres_migrator_password
export SHIN_RUNTIME_PASSWORD_FILE=/run/shin-secrets/shin_runtime_password
exec /usr/local/bin/docker-entrypoint.sh "$@"
