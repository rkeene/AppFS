AppFS
=====
It's sort of like LazyFS.

Usage
-----
Run:
	1. # mkdir /tmp/appfs-cache /opt/appfs
	2. # appfsd /opt/appfs


Paths
-----
    AppFS should normally be mounted on "/opt/appfs".

    /opt/appfs/hostname
    	Fetches: http://hostname/appfs/index
    	Contains CSV file: hash,hashMethod,<certificateInDERFormatInHex>,<PKCS#1v1.5-signature-inDERFormatInHex>
	                   \-------------/
                                  ^- Signed data
    	Fetches: http://hostname/appfs/<hashMethod>/<hash>
    	Contains CSV file: package,version,os,cpuArch,packageManifestHash,isLatest

    /opt/appfs/hostname/package/os-cpuArch/version
    /opt/appfs/hostname/<hashMethod>/
    	Fetches: http://hostname/appfs/<hashMethod>/<packageManifestHash>
    	Contains CSV file:
    		type,time,extraData,name
    		type == directory; extraData = (null)
    		type == symlink; extraData = source
    		type == file; extraData = size,perms,fileHash

    /opt/appfs/hostname/{packageManifestHash,package/os-cpuArch/version}/file
    	Fetches: http://hostname/appfs/<hashMethod>/<fileHash>

Database
--------
    sites(hostname, hashMethod, lastUpdate, ttl)
    packages(hostname, packageManifestHash, package, version, os, cpuArch, isLatest, haveManifest)
    files(packageManifestHash, type, time, source, size, perms, fileHash, file_name, file_directory)

Resources
---------
http://appfs.rkeene.org/
