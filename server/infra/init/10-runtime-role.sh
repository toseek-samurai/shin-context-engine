#!/usr/bin/env bash
set -euo pipefail

runtime_password_file="${SHIN_RUNTIME_PASSWORD_FILE:?SHIN_RUNTIME_PASSWORD_FILE is required}"
if [[ ! -f "$runtime_password_file" || -L "$runtime_password_file" ]]; then
    echo "runtime credential file is unavailable" >&2
    exit 1
fi

runtime_password="$(<"$runtime_password_file")"
if [[ -z "$runtime_password" ]]; then
    echo "runtime credential file is empty" >&2
    exit 1
fi

{
    printf '\\set runtime_password %s\n' "$runtime_password"
    cat <<'SQL'
CREATE ROLE shin_runtime
    LOGIN
    NOSUPERUSER
    NOCREATEDB
    NOCREATEROLE
    NOINHERIT
    NOREPLICATION
    NOBYPASSRLS
    PASSWORD :'runtime_password';

REVOKE ALL ON DATABASE shin_server_dev FROM PUBLIC;
GRANT CONNECT ON DATABASE shin_server_dev TO shin_runtime;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
SQL
} | psql --set=ON_ERROR_STOP=1 \
    --username "$POSTGRES_USER" \
    --dbname "$POSTGRES_DB"
