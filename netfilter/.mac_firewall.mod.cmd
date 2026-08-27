savedcmd_mac_firewall.mod := printf '%s\n'   mac_firewall.o | awk '!x[$$0]++ { print("./"$$0) }' > mac_firewall.mod
