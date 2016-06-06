#!/bin/sh
#
# keepalived   LVS cluster monitor daemon.
#
#              Written by Andres Salomon <dilinger@voxel.net>
#
### BEGIN INIT INFO
# Provides:          keepalived
# Required-Start:    $syslog $network $remote_fs
# Required-Stop:     $syslog $network $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Starts keepalived
# Description:       Starts keepalived lvs loadbalancer
### END INIT INFO
PATH=/sbin:/bin:/usr/sbin:/usr/bin
DAEMON=/usr/sbin/keepalived
NAME=keepalived
DESC=keepalived
CONFIG=/etc/keepalived/keepalived.conf
TMPFILES="/tmp/.vrrp /tmp/.healthcheckers"

#includes lsb functions 
. /lib/lsb/init-functions

test -f $CONFIG || exit 0
test -f $DAEMON || exit 0

KEEPALIVED_OPTIONS=""
DEFAULT=/etc/default/keepalived
test -r "$DEFAULT" && . "$DEFAULT"


do_stop()
{
	log_daemon_msg "Stopping $DESC" "$NAME"
	if start-stop-daemon --oknodo --stop --quiet --pidfile /var/run/$NAME.pid --exec $DAEMON; then
		log_end_msg 0
	else
		log_end_msg 1
	fi
}

do_start()
{
	log_daemon_msg "Starting $DESC" "$NAME"
	for file in $TMPFILES
	do
		test -e $file && test ! -L $file && rm $file
	done
	if start-stop-daemon --start --quiet --pidfile /var/run/$NAME.pid --exec $DAEMON -- $KEEPALIVED_OPTIONS; then
		log_end_msg 0
	else
		log_end_msg 1
	fi
}

case "$1" in
	start)
		do_start
	;;
	stop)
		do_stop
	;;
	reload|force-reload)
		log_action_begin_msg "Reloading $DESC configuration..."
		if start-stop-daemon --stop --quiet --signal 1 --pidfile /var/run/$NAME.pid --exec $DAEMON; then
			log_end_msg 0
		else
			log_action_end_msg 1
		fi
	;;
	restart)
		do_stop
		sleep 1
		do_start
	;;
	*)
		echo "Usage: /etc/init.d/$NAME {start|stop|restart|reload|force-reload}" >&2
		exit 1
	;;
esac

exit 0
