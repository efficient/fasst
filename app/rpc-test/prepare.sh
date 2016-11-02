# Prepare for debugging

echo "Checking HRD_REGISTRY_IP"
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

echo "Removing SHM keys"
for key in `seq 1 14`; do
	sudo ipcrm -M $key 1>/dev/null 2>/dev/null
done

echo "Restarting memcached"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &

