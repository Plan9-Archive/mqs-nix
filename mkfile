ARCH=\
	k10\
	
all:V:
	for(i in $ARCH)@{
		cd $i
		mk
	}

installall install:V:
	for(i in $ARCH) @{
		cd $i
		mk install
	}

clean:V:
	for(i in $ARCH) @{
		cd $i
		mk clean
	}
