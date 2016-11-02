for scriptname in \
	autorun.sh \
	kill-all.sh \
	proc-out.sh \
	run-all.sh \
	run-remote.sh \
	check-nics.sh; do
	echo "Creating symlink for $scriptname"
	ln -s autorun_scripts/$scriptname $scriptname
done
