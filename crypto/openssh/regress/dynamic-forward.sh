#	$OpenBSD: dynamic-forward.sh,v 1.4 2004/06/22 22:55:56 dtucker Exp $
#	Placed in the Public Domain.

tid="dynamic forwarding"

FWDPORT=`expr $PORT + 1`

DATA=/bin/ls${EXEEXT}

if have_prog nc && nc -h 2>&1 | grep "proxy address" >/dev/null; then
	proxycmd="nc -x 127.0.0.1:$FWDPORT -X"
elif have_prog connect; then
	proxycmd="connect -S 127.0.0.1:$FWDPORT -"
else
	echo "skipped (no suitable ProxyCommand found)"
	exit 0
fi
trace "will use ProxyCommand $proxycmd"

start_sshd

for p in 1 2; do
	trace "start dynamic forwarding, fork to background"
	${SSH} -$p -F $OBJ/ssh_config -f -D $FWDPORT -q somehost \
		exec sh -c \'"echo \$\$ > $OBJ/remote_pid; exec sleep 444"\'

	for s in 4 5; do
	    for h in 127.0.0.1 localhost; do
		trace "testing ssh protocol $p socks version $s host $h"
		${SSH} -F $OBJ/ssh_config \
			-o "ProxyCommand ${proxycmd}${s} $h $PORT" \
			somehost cat $DATA > $OBJ/ls.copy
		test -f $OBJ/ls.copy	 || fail "failed copy $DATA"
		cmp $DATA $OBJ/ls.copy || fail "corrupted copy of $DATA"
	    done
	done

	if [ -f $OBJ/remote_pid ]; then
		remote=`cat $OBJ/remote_pid`
		trace "terminate remote shell, pid $remote"
		if [ $remote -gt 1 ]; then
			kill -HUP $remote
		fi
	else
		fail "no pid file: $OBJ/remote_pid"
	fi

	# Must allow time for connection tear-down
	sleep 2
done
