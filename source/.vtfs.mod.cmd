savedcmd_source/vtfs.mod := printf '%s\n'   source/vtfs.o | awk '!x[$$0]++ { print("./"$$0) }' > source/vtfs.mod
