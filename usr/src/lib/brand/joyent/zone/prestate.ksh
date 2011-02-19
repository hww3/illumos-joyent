#!/bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
# Copyright 2010, 2011 Joyent, Inc.  All rights reserved.
# Use is subject to license terms.
#

unset LD_LIBRARY_PATH
PATH=/usr/bin:/usr/sbin
export PATH

. /lib/sdc/config.sh

# state
# ZONE_STATE_CONFIGURED		0 (script will never see this)
# ZONE_STATE_INCOMPLETE		1 (script will never see this)
# ZONE_STATE_INSTALLED		2
# ZONE_STATE_READY		3
# ZONE_STATE_RUNNING		4
# ZONE_STATE_SHUTTING_DOWN	5
# ZONE_STATE_DOWN		6
# ZONE_STATE_MOUNTED		7

# cmd
#
# ready				0
# boot				1
# halt				4

ZONENAME=$1
ZONEPATH=$2
state=$3
cmd=$4
ALTROOT=$5

# We only do work if we're readying the zone.
if [ $cmd -ne 0 ]; then
	exit 0;
fi

# We're readying the zone.  Make sure the per-zone writable
# directories exist so that we can lofs mount them.  We do this here,
# instead of in the install script, since this list has evolved and there
# are already zones out there in the installed state.
[ ! -d $ZONEPATH/site ] && mkdir -m755 $ZONEPATH/site
[ ! -d $ZONEPATH/local ] && mkdir -m755 $ZONEPATH/local
[ ! -d $ZONEPATH/root/checkpoints ] &&  mkdir -m755 $ZONEPATH/root/checkpoints

# Force zone snapshots to get mounted
ls $ZONEPATH/.zfs/snapshot/* >/dev/null 2>&1

#
# Finish by setting up the vnic(s) for the zone.
#

# Load sysinfo variables with SYSINFO_ prefix
load_sdc_sysinfo
# Load config variables with CONFIG_ prefix, and sets the headnode variable
load_sdc_config

for nic in $_ZONECFG_net_resources
do
	# Get simplified versions of the network config. variables.
	address=$(eval echo \$_ZONECFG_net_${nic}_address)
	dhcp_server=$(eval echo \$_ZONECFG_net_${nic}_dhcp_server)
	global_nic=$(eval echo \$_ZONECFG_net_${nic}_global_nic)
	mac_addr=$(eval echo \$_ZONECFG_net_${nic}_mac_addr)
	vlan_id=$(eval echo \$_ZONECFG_net_${nic}_vlan_id)
	blocked_outgoing_ports=$(eval echo \$_ZONECFG_net_${nic}_blocked_outgoing_ports)

	# If address set, must be a shared stack zone
	[[ -n $address ]] && exit 0

	# If no global-nic, must be a dedicated physical NIC instead of a vnic
	[[ -z $global_nic ]] && continue

	orig_global=$global_nic
	[[ "$global_nic" == "admin" ]] && global_nic=$SYSINFO_NIC_admin
	[[ "$global_nic" == "external" ]] && global_nic=$SYSINFO_NIC_external

	# For backwards compatibility with the other parts of the
	# system, check if this zone already has this vnic setup.
	# If so, move on to the next vnic.
	dladm show-vnic -p -o LINK $nic >/dev/null 2>&1
	(( $? == 0 )) && continue

	#
	# Create the vnic.
	#

	opt_str=""
	[[ -n $mac_addr ]] && opt_str="$opt_str -m $mac_addr"

	[[ -n $vlan_id && $vlan_id != 0 ]] && opt_str="$opt_str -v $vlan_id"

	dladm create-vnic -l $global_nic $opt_str $nic
	(( $? != 0 )) && exit 1

	if [[ -z $mac_addr ]]; then
		# There was no assigned mac address

		# Get newly assigned mac address.
		mac_addr=$(dladm show-vnic -p -o MACADDRESS ${nic})

		# Save newly assigned mac address
		[[ -n $mac_addr ]] && zonecfg -z $ZONENAME \
		    "select net physical=$nic; " \
		    "set mac-addr=$mac_addr; end; exit"
	fi

	if [[ -n $dhcp_server ]]; then
		spoof_opts="mac-nospoof"
	else
		# XXX For backwards compatibility, special handling for zone
		# named "dhcpd".  Remove this check once property is added to
		# zone.
		if [[ $ZONENAME == "dhcpd" ]]; then
		    spoof_opts="mac-nospoof"
		else
		    # Enable full antispoof if the zone is not a dhcp server.
		    spoof_opts="ip-nospoof,mac-nospoof,restricted,dhcp-nospoof"
		fi
	fi
	dladm set-linkprop -p "protection=${spoof_opts}" ${nic}

	# Get the static IP for the vnic from the zone config file.
	hostname_file="/zones/$ZONENAME/root/etc/hostname.$nic"
	if [ -e $hostname_file ]; then
		zone_ip=`nawk '{if ($1 ~ /[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/)
		    print $1}' $hostname_file`

		[[ -n "${zone_ip}" ]] && \
		    dladm set-linkprop -p "allowed-ips=${zone_ip}" ${nic}

	fi

	# If on VMWare and we have external IPs, create a bridge to allow
	# zones to reach the external gateway
	if [[ ${headnode} == "true" && ${orig_global} == "external" && "${SYSINFO_Product}" == "VMware Virtual Platform" ]]; then
		dladm show-bridge -p -o BRIDGE vmwareextbr >/dev/null 2>&1
		(( $? != 0 )) && dladm create-bridge \
		    -l ${SYSINFO_NIC_external} vmwareextbr
	fi

	if [[ -n $blocked_outgoing_ports ]]; then
		OLDIFS=$IFS
		IFS=,
		for port in $blocked_outgoing_ports; do
			# br='block remote'.  Flow names should be < 31 chars in length so
			# that they get unique kstats
			flowadm add-flow -l $nic -a transport=tcp,remote_port=$port \
			    -p maxbw=0 ${nic}_br_${port}
		done
		IFS=$OLDIFS
	fi

done

exit 0
