#!/bin/sh
set -eu

: "${ICECAST_SOURCE_PASSWORD:?Set ICECAST_SOURCE_PASSWORD}"
: "${ICECAST_RELAY_PASSWORD:?Set ICECAST_RELAY_PASSWORD}"
: "${ICECAST_ADMIN_PASSWORD:?Set ICECAST_ADMIN_PASSWORD}"
: "${ICECAST_ADMIN_EMAIL:=admin@example.com}"
: "${ICECAST_HOSTNAME:=localhost}"
: "${ICECAST_LOCATION:=AWS}"

escape_sed() {
    printf '%s' "$1" | sed 's/[&|]/\\&/g'
}

sed \
    -e "s|__ICECAST_SOURCE_PASSWORD__|$(escape_sed "$ICECAST_SOURCE_PASSWORD")|g" \
    -e "s|__ICECAST_RELAY_PASSWORD__|$(escape_sed "$ICECAST_RELAY_PASSWORD")|g" \
    -e "s|__ICECAST_ADMIN_PASSWORD__|$(escape_sed "$ICECAST_ADMIN_PASSWORD")|g" \
    -e "s|__ICECAST_ADMIN_EMAIL__|$(escape_sed "$ICECAST_ADMIN_EMAIL")|g" \
    -e "s|__ICECAST_HOSTNAME__|$(escape_sed "$ICECAST_HOSTNAME")|g" \
    -e "s|__ICECAST_LOCATION__|$(escape_sed "$ICECAST_LOCATION")|g" \
    /etc/icecast2/icecast.xml.template > /etc/icecast2/icecast.xml

exec icecast2 -c /etc/icecast2/icecast.xml
