#!/sbin/sh

# Requirements:

# - a modded android in recovery mode (JF 1.3 will work by default)
# - adb shell as root in recovery mode if not using a pre-made recovery image
# - busybox in recovery mode
# - dump_image-arm-uclibc compiled and in path on phone
# - mkyaffs2image-arm-uclibc compiled and installed in path on phone
# - flash_image-arm-uclibc compiled and in path on phone
# - unyaffs-arm-uclibc compiled and in path on phone
# - for [de]compression needs gzip or bzip2, part of the busybox
# - wget for the wireless updates

# dev:    size   erasesize  name
#mtd0: 00040000 00020000 "misc"
#mtd1: 00500000 00020000 "recovery"
#mtd2: 00280000 00020000 "boot"
#mtd3: 04380000 00020000 "system"
#mtd4: 04380000 00020000 "cache"
#mtd5: 04ac0000 00020000 "userdata"


SUBNAME=""
NOBOOT=0
NODATA=0
NOSYSTEM=0
NOSECURE=0

RESTORE=0
BACKUP=0
INSTALL_ROM=0

BACKUPPATH="/sdcard/nandroid/"


# Boot, Data, System, Android-secure
# Enables the user to figure at a glance what is in the backup
BACKUPLEGEND=""

# Normally we want tar to be verbose for confidence building.
TARFLAGS="v"

ASSUMEDEFAULTUSERINPUT=0

sd_mounted=`mount | grep "/sdcard" | wc -l`
data_mounted=`mount | grep "/data" | wc -l`
system_mounted=`mount | grep "/system" | wc -l`

	
echo2log()
{
    if [ -e /cache/recovery/log ]; then
	echo $1 >>/cache/recovery/log
    else
	echo $1 >/cache/recovery/log
    fi
}

batteryAtLeast()
{
    REQUIREDLEVEL=$1
    ENERGY=`cat /sys/class/power_supply/battery/capacity`
    if [ "`cat /sys/class/power_supply/battery/status`" == "Charging" ]; then
	ENERGY=100
    fi
    if [ ! $ENERGY -ge $REQUIREDLEVEL ]; then
	echo "* print Error: not enough battery power, need at least $REQUIREDLEVEL%."
	echo "* print Connect charger or USB power and try again"
	exit 1
    fi
}

if [ "`echo $0 | grep /sbin/nandroid-mobile.sh`" == "" ]; then
    cp $0 /sbin
    chmod 755 /sbin/`basename $0`
    exec /sbin/`basename $0` $@
fi


# Hm, have to handle old options for the current UI
case $1 in
    restore)
        shift
        RESTORE=1
        ;;
    backup)
        shift
        BACKUP=1
        ;;
    --)
        ;;
esac

ECHO=echo
OUTPUT=""

for option in $(getopt --name="nandroid-mobile v2.2.1" -l progress -l install-rom: -l noboot -l nodata -l nosystem -l nosecure -l subname: -l backup -l restore -l defaultinput -- "cbruds:p:eqli:" "$@"); do
    case $option in
	--verbose)
	    VERBOSE=1
	    shift
	    ;;
        --noboot)
            NOBOOT=1
            #$ECHO "No boot"
            shift
            ;;
        --nodata)
            NODATA=1
            #$ECHO "No data"
            shift
            ;;
        --nosystem)
            NOSYSTEM=1
            #$ECHO "No system"
            shift
            ;;
        --nosecure)
            NOSECURE=1
            #$ECHO "No secure"
            shift
            ;;
        --backup)
            BACKUP=1
            #$ECHO "backup"
            shift
            ;;
        -b)
            BACKUP=1
            #$ECHO "backup"
            shift
            ;;
			
        --restore)
            RESTORE=1
            #$ECHO "restore"
            shift
            ;;
			
        --defaultinput)
            ASSUMEDEFAULTUSERINPUT=1
            shift
            ;;
        -r)
            RESTORE=1
            #$ECHO "restore"
            shift
            ;;
        --subname)
            if [ "$2" == "$option" ]; then
                shift
            fi
            #$ECHO $2
            SUBNAME="$2"
            shift
            ;;
        --defaultinput)
            ASSUMEDEFAULTUSERINPUT=1
            shift
            ;;
        -s)
            if [ "$2" == "$option" ]; then
                shift
            fi
            #$ECHO $2
            SUBNAME="$2"
            shift
            ;;
	--install-rom)
	    if [ "$2" == "$option" ]; then
		shift
	    fi
	    INSTALL_ROM=1
	    ROM_FILE=$2
	    ;;
	--progress)
	    PROGRESS=1
	    ;;
        --)
            shift
            break
            ;;
    esac
done

echo "* print "
echo "* print nandroid-mobile v2.2.1 (RZ Hack)"
echo "* print "
##

[ "$PROGRESS" == "1" ] && echo "* show_indeterminate_progress"

pipeline() {
    if [ "$PROGRESS" == "1" ]; then
	echo "* show_indeterminate_progress"
	awk "NR==1 {print \"* ptotal $1\"} {print \"* pcur \" NR}"
    else
	cat
    fi
}

# Make sure it exists
touch /cache/recovery/log

if [ ! "$SUBNAME" == "" ]; then
    if [ "$BACKUP" == 1 ]; then
        echo "* print Using $SUBNAME- prefix to create a backup folder"
    else
        if [ "$RESTORE" == 1 ]; then
            echo "* print Searching for backup directories, matching $SUBNAME, restore."
            echo "* print "
        fi
    fi
else
    if [ "$BACKUP" == 1 ]; then
        if [ "$ASSUMEDEFAULTUSERINPUT" == 0 ]; then
            read SUBNAME
        fi
        echo "* print "
        echo "* print Using $SUBNAME- prefix to create a backup folder"
        echo "* print "
    else
        if [ "$RESTORE" == 1 -o "$DELETE" == 1 ]; then
            if [ "$ASSUMEDEFAULTUSERINPUT" == 0 ]; then
                read SUBNAME
            fi

        fi
    fi
fi

if [ "$BACKUP" == 1 ]; then
    if [ "$VERBOSE" == "1" ]; then
        tar="busybox tar cvf"
    else
	tar="busybox tar cf"
    fi
	
    mkyaffs2image=`which mkyaffs2image`
    if [ "$mkyaffs2image" == "" ]; then
	mkyaffs2image=`which mkyaffs2image-arm-uclibc`
	if [ "$mkyaffs2image" == "" ]; then
	    echo "* print error: mkyaffs2image or mkyaffs2image-arm-uclibc not found in path"
	    exit 6
	fi
    fi
    dump_image=`which dump_image`
    if [ "$dump_image" == "" ]; then
	dump_image=`which dump_image-arm-uclibc`
	if [ "$dump_image" == "" ]; then
	    echo "* print error: dump_image or dump_image-arm-uclibc not found in path"
	    exit 7
	fi
    fi
fi

if [ "$RESTORE" == 1 ]; then
    if [ "$VERBOSE" == "1" ]; then
		untar="busybox tar xvf"
    else
        untar="busybox tar xf"
    fi

    flash_image=`which flash_image`
    if [ "$flash_image" == "" ]; then
		flash_image=`which flash_image-arm-uclibc`
	if [ "$flash_image" == "" ]; then
	    echo "* print error: flash_image or flash_image-arm-uclibc not found in path"
	    exit 8
	fi
    fi
    unyaffs=`which unyaffs`
    if [ "$unyaffs" == "" ]; then
		unyaffs=`which unyaffs-arm-uclibc`
	if [ "$unyaffs" == "" ]; then
	    echo "* print error: unyaffs or unyaffs-arm-uclibc not found in path"
	    exit 9
	fi
    fi
fi

if [ "$INSTALL_ROM" == 1 ]; then
	sd_mounted=`mount | grep "/sdcard" | wc -l`
	data_mounted=`mount | grep "/data" | wc -l`
	system_mounted=`mount | grep "/system" | wc -l`
	
    batteryAtLeast 30

    $ECHO "* show_indeterminate_progress"

    echo "* print Installing ROM from $ROM_FILE..."

	if [ "$sd_mounted" == "0" ]; then
		mount /sdcard
	fi

    if [ -z "$(mount | grep sdcard)" ]; then
	echo "* print error: unable to mount /sdcard, aborting"
	exit 13
    fi
    
    if [ ! -f $ROM_FILE ]; then
	echo "* print error: specified ROM file does not exist"
	exit 14
    fi

    if echo "$ROM_FILE" | grep ".*gz" >/dev/null;
    then
		$ECHO "* print Decompressing ROM..."
		echo "* print Decompressing $ROM_FILE..."
		busybox gzip -d "$ROM_FILE"
		ROM_FILE=$(echo "$ROM_FILE" | sed -e 's/.tar.gz$/.tar/' -e 's/.tgz/.tar/')
		echo "* print Decompressed to $ROM_FILE"
    fi

    cd /tmp
    TAR_OPTS="x"
    [ "$PROGRESS" == "1" ] && TAR_OPTS="${TAR_OPTS}v"
    TAR_OPTS="${TAR_OPTS}f"
    
    [ "$PROGRESS" == "1" ] && $ECHO "* print Unpacking $ROM_FILE..."
    PTOTAL=$(($(tar tf "$ROM_FILE" | wc -l)-2))
	echo "* print $PTOTAL files in archive!"
	echo "* print "
    
    tar $TAR_OPTS "$ROM_FILE" --exclude ./system.tar --exclude ./data.tar | pipeline $PTOTAL

    if [ -z $(tar tf "$ROM_FILE" | grep \./data\.tar) ]; then
	NODATA=1
    fi

    if [ -z $(tar tf "$ROM_FILE" | grep \./system\.tar) ]; then
	NOSYSTEM=1
    fi

    GETVAL=sed\ -r\ 's/.*=(.*)/\1/'

    if [ -d metadata ]; then
	HGREV=$(cat /recovery.version | awk '{print $2}')
	MIN_REV=$(cat metadata/* | grep "min_rev=" | head -n1 | $GETVAL)
	
	[ "$HGREV" -ge "$MIN_REV" ] || ($ECHO "ERROR: your installed version of the recovery image is too old. Please upgrade."; exit 1) || exit 154

	echo "* print "
	echo "* print --==AUTHORS==--"
	echo "* print  $(cat metadata/* | grep "author=" | $GETVAL)"
	echo "* print --==NAME==--"
	echo "* print  $(cat metadata/* | grep "name=" | $GETVAL)"
	echo "* print --==DESCRIPTION==--"
	echo "* print  $(cat metadata/* | grep "description=" | $GETVAL)"
	echo "* print --==HELP==--"
	echo "* print  $(cat metadata/* | grep "help=" | $GETVAL)"
	echo "* print --==URL==--"
	echo "* print  $(cat metadata/* | grep "url=" | $GETVAL)"
	echo "* print --==EMAIL==--"
	echo "* print $(cat metadata/* | grep "email=" | $GETVAL)"
	echo "* print "
    fi

    if [ -d pre.d ]; then
		echo "* print Executing pre-install scripts..."
		for sh in pre.d/[0-9]*.sh; do
			if [ -r "$sh" ]; then
			. "$sh"
			fi
		done
    fi

    for image in system data; do
	if [ "$image" == "data" -a "$NODATA" == "1" ];
	then
	    echo "* print Not flashing /data"
	    continue
	fi

	if [ "$image" == "system" -a "$NOSYSTEM" == "1" ];
	then
	    echo "* print Not flashing /system"
	    continue
	fi
	
	echo "* print Flashing /$image from $image.tar"
	
	is_mounted=`mount | grep $image | wc -l`
	if [ "$is_mounted" != "1" ]; then
		mount /$image 2>/dev/null
	fi
	
	if [ "$(mount | grep $image)" == "" ]; then
	    echo "* print error: unable to mount /$image"
	    exit 16
	fi

	if cat metadata/* | grep "^clobber_${image}=" | grep "true" > /dev/null; then
	    echo "* print wiping /${image}..."
	    rm -r /$image/* 2>/dev/null  
	fi

	cd /$image

	TAR_OPTS="x"
	[ "$PROGRESS" == "1" ] && TAR_OPTS="${TAR_OPTS}v"
	PTOTAL=$(tar xOf "$ROM_FILE" ./${image}.tar | tar t | wc -l)
	[ "$PROGRESS" == "1" ] && $ECHO "* print Extracting ${image}.tar"

	tar xOf "$ROM_FILE" ./${image}.tar | tar $TAR_OPTS | pipeline $PTOTAL
    done

    cd /tmp

    if [ -d post.d ]; then
	echo "* print Executing post-install scripts..."
	for sh in post.d/[0-9]*.sh; do
	    if [ -r "$sh" ]; then
		. "$sh"
	    fi
	done
    fi

    echo "* print Installed ROM from $ROM_FILE"
	
	if [ "$system_mounted" != "0" ]; then
		umount /system 2>/dev/null
	fi
	if [ "$data_mounted" != "0" ]; then
		umount /data 2>/dev/null
	fi
    exit 0
fi

if [ "$RESTORE" == 1 ]; then
	
	sd_mounted=`mount | grep "/sdcard" | wc -l`
	data_mounted=`mount | grep "/data" | wc -l`
	system_mounted=`mount | grep "/system" | wc -l`
    batteryAtLeast 30
	
	if [ "$sd_mounted" == "0" ]; then
		umount /sdcard 2>/dev/null
		mount /sdcard 2>/dev/null
	fi
	
    if [ "$sd_mounted" == "0" ]; then
		echo "* print error: unable to mount /sdcard, aborting"
		exit 20
    fi



    RESTOREPATH=`ls -trd $BACKUPPATH/*$SUBNAME* 2>/dev/null | tail -1`
    echo "* print  "

    if [ "$RESTOREPATH" = "" ]; then
		echo "* print Error: no backups found"
		exit 21
    else
        echo "* print Default backup is the latest: $RESTOREPATH"
        ls -trd $BACKUPPATH/*$SUBNAME* 2>/dev/null | grep -v $RESTOREPATH $OUTPUT
        echo "* print "

        if [ "$ASSUMEDEFAULTUSERINPUT" == 0 ]; then
            read SUBSTRING
        else
            SUBSTRING=""
        fi
        echo "* print "

        if [ ! "$SUBSTRING" == "" ]; then
            RESTOREPATH=`ls -trd $BACKUPPATH/*$SUBNAME* 2>/dev/null | grep $SUBSTRING | tail -1`
        else
            RESTOREPATH=`ls -trd $BACKUPPATH/*$SUBNAME* 2>/dev/null | tail -1`
        fi
        if [ "$RESTOREPATH" = "" ]; then
            echo "* print Error: no matching backups found, aborting"
            exit 22
        fi
    fi
    
    echo "* print Restore path: $RESTOREPATH"
    echo "* print "
	
	if [ "$system_mounted" == "0" ]; then
		mount /system 2>/dev/null
	fi
	system_mounted=`mount | grep "/system" | wc -l`
    if [ "$data_mounted" == "0" ]; then
		mount /data 2>/dev/null
	fi
	if [ -z "$(mount | grep data)" ]; then
		echo "* print error: unable to mount /data, aborting"	
		exit 23
    fi
    if [ -z "$(mount | grep system)" ]; then
		echo "* print error: unable to mount /system, aborting"	
		exit 24
    fi
    
    CWD=$PWD
    cd $RESTOREPATH

    echo "* print Processing backup images..."

    if [ `ls boot* 2>/dev/null | wc -l` == 0 ]; then
        NOBOOT=1
    fi
    if [ `ls data* 2>/dev/null | wc -l` == 0 ]; then
        NODATA=1
    fi
    if [ `ls system* 2>/dev/null | wc -l` == 0 ]; then
        NOSYSTEM=1
    fi
	if [ `ls secure* 2>/dev/null | wc -l` == 0 ]; then
        NOSECURE=1
    fi

    for image in boot; do
        if [ "$NOBOOT" == "1" -a "$image" == "boot" ]; then
            echo "* print "
            echo "* print Not flashing boot image!"
            echo "* print "
            continue
        fi
        echo "* print Flashing $image..."
		$flash_image $image $image.img $OUTPUT
		echo "* print done."
    done

    for image in data system secure; do
        if [ "$NODATA" == 1 -a "$image" == "data" ]; then
            echo "* print "
            echo "* print Not restoring data image!"
            echo "* print "
            continue
        elif [ "$NODATA" == 0 -a "$image" == "data" ]; then
			image="data"	
			tarfile="data"
		fi
        if [ "$NOSYSTEM" == 1 -a "$image" == "system" ]; then
            echo "* print "
            echo "* print Not restoring system image!"
            echo "* print "
            continue
        elif [ "$NOSYSTEM" == 0 -a "$image" == "system" ]; then
			image="system"	
			tarfile="system"
		fi
        if [ "$NOSECURE" == 1 -a "$image" == "secure" ]; then
            echo "* print "
            echo "* print Not restoring .android_secure!"
            echo "* print "
            continue
        elif [ "$NOSECURE" == 0 -a "$image" == "secure" ]; then
			image="sdcard/.android_secure"	
			tarfile="secure"
		fi
		echo "* print Erasing /$image..."
		cd /$image
		rm -rf * 2>/dev/null
		
		TAR_OPTS="x"
		[ "$PROGRESS" == "1" ] && TAR_OPTS="${TAR_OPTS}v"
		TAR_OPTS="${TAR_OPTS}f"

		PTOTAL=$(tar tf $RESTOREPATH/$image.tar | wc -l)
		[ "$PROGRESS" == "1" ] && $ECHO "* print Unpacking $image..."

		tar $TAR_OPTS $RESTOREPATH/$tarfile.tar -C /$image | pipeline $PTOTAL
		if [ "$image" == "data" ]; then
			rm /data/misc/ril/pppd-notifier.fifo
		fi

		cd /
		sync
		is_mounted=`mount | grep $image | wc -l`
		if [ "$is_mounted" != "0" ]; then
			umount /$image 2> /dev/null
		fi
    echo "* print done."
    done
    
    exit 0
    echo "* print Thanks for using RZRecovery."
fi


# 2.
if [ "$BACKUP" == 1 ]; then
	sd_mounted=`mount | grep "/sdcard" | wc -l`
	data_mounted=`mount | grep "/data" | wc -l`
	system_mounted=`mount | grep "/system" | wc -l`
	
    TAR_OPTS="c"
    [ "$PROGRESS" == "1" ] && TAR_OPTS="${TAR_OPTS}v"
    TAR_OPTS="${TAR_OPTS}f"


    echo "* print mounting system and data read-only, sdcard read-write"
	
	if [ "$system_mounted" != "0" ]; then
		umount /system 2>/dev/null
	fi
	if [ "$data_mounted" != "0" ]; then
		umount /data 2>/dev/null
	fi
	if [ "$sd_mounted" != "0" ]; then
		umount /sdcard 2>/dev/null
	fi
	if [ "$system_mounted" == "0" ]; then
		mount /system 
	fi
	if [ "$data_mounted" == "0" ]; then
		mount /data 
	fi
	if [ "$sd_mounted" != "0" ]; then
		mount /sdcard 2> /dev/null 
	fi
    if [ ! "$SUBNAME" == "" ]; then
	SUBNAME=$SUBNAME-
    fi

# Identify the backup with what partitions have been backed up
    if [ "$NOBOOT" == 0 ]; then
	BACKUPLEGEND=$BACKUPLEGEND"B"
    fi
    if [ "$NODATA" == 0 ]; then
	BACKUPLEGEND=$BACKUPLEGEND"D"
    fi
    fi
    if [ "$NOSYSTEM" == 0 ]; then
	BACKUPLEGEND=$BACKUPLEGEND"S"
    fi
	if [ "$NOSECURE" == 0 ]; then
	BACKUPLEGEND=$BACKUPLEGEND"A"
    fi
    if [ ! "$BACKUPLEGEND" == "" ]; then
	BACKUPLEGEND=$BACKUPLEGEND-
    fi


    TIMESTAMP="`date +%Y%m%d-%H%M`"
    DESTDIR="$BACKUPPATH/$SUBNAME$BACKUPLEGEND$TIMESTAMP"
    if [ ! -d $DESTDIR ]; then 
	mkdir -p $DESTDIR
	if [ ! -d $DESTDIR ]; then 
	    echo "* print error: cannot create $DESTDIR"
		if [ "$system_mounted" != "0" ]; then
			umount /system 2>/dev/null
		fi
		if [ "$data_mounted" != "0" ]; then
			umount /data 2>/dev/null
		fi
		if [ "$sd_mounted" != "0" ]; then
			umount /sdcard 2>/dev/null
		fi
	    exit 32
#	fi
    else
		touch $DESTDIR/.nandroidwritable
		if [ ! -e $DESTDIR/.nandroidwritable ]; then
			echo "* print error: cannot write to $DESTDIR"
			if [ "$system_mounted" != "0" ]; then
				umount /system 2>/dev/null
			fi
			if [ "$data_mounted" != "0" ]; then
				umount /data 2>/dev/null
			fi
			if [ "$sd_mounted" != "0" ]; then
				umount /sdcard 2>/dev/null
			fi
			exit 33
		fi
		rm $DESTDIR/.nandroidwritable
    fi

# 3.
    echo "* print checking free space on sdcard"
    FREEBLOCKS="`df -k /sdcard| grep sdcard | awk '{ print $4 }'`"
# we need about 130MB for the dump
    if [ $FREEBLOCKS -le 130000 ]; then
	echo "* print Error: not enough free space available on sdcard (need 130mb), aborting."
		if [ "$system_mounted" != "0" ]; then
			umount /system 2>/dev/null
		fi
		if [ "$data_mounted" != "0" ]; then
			umount /data 2>/dev/null
		fi
		if [ "$sd_mounted" != "0" ]; then
			umount /sdcard 2>/dev/null
		fi
	exit 34
    fi


# 5.
    for image in boot; do

	case $image in
            boot)
		if [ "$NOBOOT" == 1 ]; then
                    echo "* print Dump of the boot partition suppressed."
                    continue
		fi
		;;
	esac

	DEVICEMD5=`$dump_image $image - | md5sum | awk '{ print $1 }'`
	sleep 1s
	MD5RESULT=1
	# 5b
	echo "* print Dumping $image..."
	ATTEMPT=0
	while [ $MD5RESULT -eq 1 ]; do
	    let ATTEMPT=$ATTEMPT+1
		# 5b1
	    $dump_image $image $DESTDIR/$image.img $OUTPUT
	    sync
		# 5b3
	    echo "${DEVICEMD5}  $DESTDIR/$image.img" | md5sum -c -s - $OUTPUT
	    if [ $? -eq 1 ]; then
		true
	    else
		MD5RESULT=0
	    fi
	    if [ "$ATTEMPT" == "5" ]; then
		echo "* print Fatal error while trying to dump $image, aborting."
		if [ "$system_mounted" != "0" ]; then
			umount /system 2>/dev/null
		fi
		if [ "$data_mounted" != "0" ]; then
			umount /data 2>/dev/null
		fi
		if [ "$sd_mounted" != "0" ]; then
			umount /sdcard 2>/dev/null
		fi
		exit 35
	    fi
	done
	echo "* print done."
    done

# 6
    for image in system data secure; do
	case $image in
            system)
		if [ "$NOSYSTEM" == 1 ]; then
                    echo "* print Dump of the system partition suppressed."
                    continue
		elif [ "$NOSYSTEM" == 0 ]; then
					dest="system"
					image="system"
		fi
		;;
            data)
		if [ "$NODATA" == 1 ]; then
                    echo "* print Dump of the data partition suppressed."
                    continue
		elif [ "$NODATA" != 1 ]; then
					dest="data"
					image="data"
		fi
		;;
            secure)
		if [ "$NOSECURE" == 1 ]; then
                    echo "* print Dump of .android_secure suppressed."
                    continue
		elif [ "$NOSECURE" != 1 ]; then
					image="/sdcard/.android_secure"
					dest="secure"
		fi
		;;
	esac

	echo "* print Dumping $image..."

	cd /$image

	PTOTAL=$(find . | wc -l)
	[ "$PROGRESS" == "1" ]

	tar $TAR_OPTS $DESTDIR/$dest.tar . 2>/dev/null | pipeline $PTOTAL
	

	sync
	echo "* print $image backup complete!"

    done

    echo "* print unmounting system, data and sdcard"
		if [ "$system_mounted" != "0" ]; then
			umount /system 2>/dev/null
		fi
		if [ "$data_mounted" != "0" ]; then
			umount /data 2>/dev/null
		fi
		if [ "$sd_mounted" != "0" ]; then
			umount /sdcard 2>/dev/null
		fi

    echo "* print Backup successful."
	echo "* print Thanks for using RZRecovery."
    if [ "$AUTOREBOOT" == 1 ]; then
	reboot
    fi
    exit 0
fi