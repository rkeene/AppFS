AppFS
=====
It's sort of like LazyFS.


Paths
-----
AppFS should normally be mounted on "/opt/appfs".

/opt/appfs/hostname
	Fetches: http://hostname/appfs/index
	Contains CSV file: hash,extraData

	Fetches: http://hostname/appfs/sha1/<hash>
	Contains CSV file: package,version,os,cpuArch,sha1,isLatest

/opt/appfs/hostname/package/os-cpuArch/version
/opt/appfs/hostname/sha1/
	Fetches: http://hostname/appfs/sha1/<sha1>
	Contains CSV file:
		type,time,extraData,name
		type == directory; extraData = (null)
		type == symlink; extraData = source
		type == file; extraData = size,sha1

/opt/appfs/hostname/{sha1,package/os-cpuArch/version}/file
	Fetches: http://hostname/appfs/sha1/<sha1>

