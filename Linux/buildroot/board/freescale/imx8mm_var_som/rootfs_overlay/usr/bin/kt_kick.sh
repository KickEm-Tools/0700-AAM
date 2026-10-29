#!/bin/sh

echo "Executing kt_Kick.sh"
sleep 15
FILE=/usr/bin/KT_Kick
if test -f "$FILE"; then
	chmod 777 /usr/bin/KT_Kick
	echo "$FILE Executing KT_Kick bin."
	# exec: KT_Kick becomes ktkick.service's main process, so systemctl stop / restart
	# (KillMode=process) signals the app itself, and its exit status reaches Restart=.
	exec /usr/bin/KT_Kick
else 
	echo "$FILE KT_Kick Bin missing"
fi
