#!/bin/sh
#
# make check: the RPC of rpc/rpcl/block.x must stay compatible with the
# released versions in tests/compat/rpc/, see rpc-check.py.

srcdir=${srcdir:-$(dirname "$0")/..}
CHECK="$srcdir/compat/rpc-check.py"

for py in python3 /usr/libexec/platform-python; do
	if command -v "$py" >/dev/null 2>&1; then
		PYTHON=$py
		break
	fi
done
if [ -z "$PYTHON" ]; then
	echo "rpc-check: python3 not found, skipped"
	exit 77
fi

"$PYTHON" -I "$CHECK" --self-test || exit 1
exec "$PYTHON" -I "$CHECK"
