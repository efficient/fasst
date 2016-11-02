for scriptname in \
	apt-params.sh \
	kill-all.sh \
	proc-out.sh \
	run-all.sh \
	run-remote.sh \
	check-nics.sh; do
	echo "Creating symlink for $scriptname"
	ln -s apt_scripts/$scriptname $scriptname
done
