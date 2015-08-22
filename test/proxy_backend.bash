#!/bin/bash

set -e

if [ -z "${BACKEND_HOST}" ]; then
    export BACKEND_HOST="localhost"
fi
if [ -z "${BACKEND_IP}" ]; then
    export BACKEND_IP="::1"
fi
if [ -z "${BACKEND_PORT}" ]; then
    export BACKEND_PORT="9934"
fi
: ${BACKEND_LOCK:="backend.lock"}
: ${srcdir:="."}
: ${APACHE2:="apache2"}
: ${TEST_LOCK_WAIT:="30"}

function backend_apache
{
    dir="${1}"
    conf="${2}"
    action="${3}"
    lockfile="${4}"

    if [ -n "${lockfile}" ]; then
	flock_cmd="flock -w ${TEST_LOCK_WAIT} ${lockfile}"
    fi

    TEST_NAME="$(basename "${dir}")"
    (
	export TEST_NAME
	export TEST_IP="${BACKEND_IP}"
	export TEST_PORT="${BACKEND_PORT}"
	export srcdir="$(realpath ${srcdir})"
	case $action in
	    start)
		${flock_cmd} \
		    ${APACHE2} -f "$(realpath ${testdir}/${conf})" -k start || return 1
		;;
	    stop)
		${APACHE2} -f "$(realpath ${testdir}/${conf})" -k stop || return 1
		;;
	esac
    )
}
