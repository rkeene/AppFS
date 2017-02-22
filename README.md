AppFS
=====
It's sort of like LazyFS.

Usage
-----
Run:
	1. # mkdir /tmp/appfs-cache /opt/appfs
	2. # appfsd /tmp/appfs-cache /opt/appfs


Paths
-----
    AppFS should normally be mounted on "/opt/appfs".

    /opt/appfs/hostname
    	Fetches: http://hostname/appfs/index
    	Contains CSV file: hash,hashMethod,<certificateInDERFormatInHex>,<PKCS#1v1.5-signature-inDERFormatInHex>
	                   \-------------/
                                  ^- Signed data
    	Fetches: http://hostname/appfs/sha1/<hash>
    	Contains CSV file: package,version,os,cpuArch,sha1,isLatest

    /opt/appfs/hostname/package/os-cpuArch/version
    /opt/appfs/hostname/sha1/
    	Fetches: http://hostname/appfs/sha1/<sha1>
    	Contains CSV file:
    		type,time,extraData,name
    		type == directory; extraData = (null)
    		type == symlink; extraData = source
    		type == file; extraData = size,perms,sha1

    /opt/appfs/hostname/{sha1,package/os-cpuArch/version}/file
    	Fetches: http://hostname/appfs/sha1/<sha1>

Database
--------
    packages(hostname, sha1, package, version, os, cpuArch, isLatest, haveManifest)
    files(package_sha1, type, time, source, size, perms, file_sha1, file_name, file_directory)

Resources
---------
http://appfs.rkeene.org/
