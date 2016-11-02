# Multicast instructions

 * Enable IPoIB on all machines. On machine `i` on Apt
```
	ifconfig ib0 10.0.0.i/24
```

 * Create the multicast server at machine i. This will print a fresh multicast
   GID.
```
	mckey -M 0 -b 10.0.0.i
```

 * Start the data transfer from another machine j:
```
	mckey -M <GID> -b 10.0.0.j
```
