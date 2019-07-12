/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2019 Joyent, Inc.
 */

/*
 * The sas FMRI scheme is intended to be used in conjuction with a
 * digraph-based topology to represent a SAS fabric.
 *
 * There are four types of vertices in the topology:
 *
 * initiator
 * ---------
 * An initiator is a device on the SAS fabric that originates SCSI commands.
 * Typically this is a SAS host-bus adapter (HBA) which can be built onto the
 * system board or be part of a PCIe add-in card.
 *
 * XXX - add description of initiator node properties
 *
 * port
 * ----
 * A port is a logical construct that represents a grouping of one or more
 * PHYs.  A port with one PHY is known as a narrow port.  An example of a
 * narrow port would be the connection from an expander to a target device.
 * A port with more than one PHY is known as a wide port.  A typical example
 * of a wide port would be the connection from an initiator to an exander
 * (typically 4 or 8 PHYs wide).
 *
 * XXX - add description of port node properties
 *
 * target
 * ------
 * A target (or end-device) represents the device that is receiving
 * SCSI commands from the an initiator.   Examples include disks and SSDs as
 * well as SMP and SES management devices.  SES and SMP targets would
 * be connected to an expander.  Disk/SSD targets can be connected to an
 * expander or directly attached (via a narrow port) to an initiator.
 *
 * XXX - add description of target node properties
 *
 * XXX - It'd be really cool if we could check for a ZFS pool config and
 * try to match the target to a leaf vdev and include the zfs-scheme FMRI of
 * that vdev as a property on this node.
 *
 * XXX - Similarly, for disks/ssd's it'd be cool if we could a match the
 * target to a disk node in the hc-scheme topology and also add the
 * hc-scheme FMRI of that disk as a property on this node.  This one would
 * have to be a dynamic (propmethod) property because we'd need to walk
 * the hc-schem tree, which may not have been built when we're enumerating.
 *
 * expander
 * --------
 * An expander acts as both a port multiplexer and expander routing signals
 * between one or more initiators and one or more targets or possibly a
 * second layer of downstream expanders, depending on the size of the fabric.
 * The SAS specification optionally allows for up to two levels of expanders
 * between the initiator(s) and target(s).
 *
 * XXX - add description of expander node properties
 *
 * Version 0 sas FMRI scheme
 * -------------------------
 * The resource in the sas FMRI scheme doesn't represent a discrete component
 * like the hc or svc schemes.  Rather, the resource represents a unique
 * path from a given initiator to a given target.  Hence, the first two
 * node/instance pairs are always an initiator and port and last two pairs
 * are always a port and a target. In between there may be one or two sets
 * of expander and port pairs.
 *
 * e.g.
 * sas://<auth>/initiator=<inst>/<port>=<inst>/.../port=<inst>/target=<inst>
 *
 * Node instance numbers are based on the local SAS address of the underlying
 * component.  Each initiator, expander and target will have a unique[1] SAS
 * address.  And each port from an initiator or to a target will also have a
 * unique SAS address.  However, expander ports are not individually
 * addressed.  If the expander port is attached, the instance number shall
 * be the SAS address of the attached device.  If the expander port is not
 * attached, the instance number shall be the SAS address of the expander,
 * itself.
 *
 * [1] The SAS address will be unique within a given SAS fabric (domain)
 *
 * The nvlist representation of the FMRI consists of two nvpairs:
 *
 * name               type                   value
 * ----               ----                   -----
 * sas-fmri-version   DATA_TYPE_UINT8        0
 * sas-path           DATA_TYPE_NVLIST_ARRAY see below
 *
 * sas-path is an array of nvlists where each nvlist contains the following
 * nvpairs:
 *
 * name               type                   value
 * ----               ----                   -----
 * sas-name           DATA_TYPE_STRING       (initiator|port|expander|target)
 * sas-id             DATA_TYPE_UINT64       SAS address (see above)
 *
 * XXX - what, if anything, should we put in the FMRI authority?
 */
#include <libnvpair.h>
#include <fm/topo_mod.h>

#include <sys/fm/protocol.h>
#include <sys/types.h>

#include <topo_digraph.h>
#include <topo_sas.h>
#include <topo_method.h>
#include <topo_subr.h>
#include "sas.h"

#include <smhbaapi.h>

static int sas_fmri_nvl2str(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
static int sas_fmri_str2nvl(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
static int sas_dev_fmri(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
static int sas_hc_fmri(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);

static const topo_method_t sas_methods[] = {
	{ TOPO_METH_NVL2STR, TOPO_METH_NVL2STR_DESC, TOPO_METH_NVL2STR_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_fmri_nvl2str },
	{ TOPO_METH_STR2NVL, TOPO_METH_STR2NVL_DESC, TOPO_METH_STR2NVL_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_fmri_str2nvl },
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC, TOPO_METH_SAS2DEV_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_dev_fmri },
	{ TOPO_METH_SAS2HC, TOPO_METH_SAS2HC_DESC, TOPO_METH_SAS2HC_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_hc_fmri },
	{ NULL }
};

static int sas_enum(topo_mod_t *, tnode_t *, const char *, topo_instance_t,
    topo_instance_t, void *, void *);
static void sas_release(topo_mod_t *, tnode_t *);

static const topo_modops_t sas_ops =
	{ sas_enum, sas_release };

static const topo_modinfo_t sas_info =
	{ "sas", FM_FMRI_SCHEME_SAS, SAS_VERSION, &sas_ops };

int
sas_init(topo_mod_t *mod, topo_version_t version)
{
	if (getenv("TOPOSASDEBUG"))
		topo_mod_setdebug(mod);
	topo_mod_dprintf(mod, "initializing sas builtin\n");

	if (version != SAS_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (topo_mod_register(mod, &sas_info, TOPO_VERSION) != 0) {
		topo_mod_dprintf(mod, "failed to register sas_info: "
		    "%s\n", topo_mod_errmsg(mod));
		return (-1);
	}

	return (0);
}

void
sas_fini(topo_mod_t *mod)
{
	topo_mod_unregister(mod);
}

static topo_vertex_t *
sas_create_vertex(topo_mod_t *mod, const char *name, topo_instance_t inst)
{
	topo_vertex_t *vtx;
	tnode_t *tn;
	topo_pgroup_info_t pgi;
	int err;

	pgi.tpi_namestab = TOPO_STABILITY_PRIVATE;
	pgi.tpi_datastab = TOPO_STABILITY_PRIVATE;
	pgi.tpi_version = TOPO_VERSION;
	if (strcmp(name, TOPO_VTX_EXPANDER) == 0)
		pgi.tpi_name = TOPO_PGROUP_EXPANDER;
	else if (strcmp(name, TOPO_VTX_INITIATOR) == 0)
		pgi.tpi_name = TOPO_PGROUP_INITIATOR;
	else if (strcmp(name, TOPO_VTX_PORT) == 0)
		pgi.tpi_name = TOPO_PGROUP_SASPORT;
	else if (strcmp(name, TOPO_VTX_TARGET) == 0)
		pgi.tpi_name = TOPO_PGROUP_TARGET;
	else {
		topo_mod_dprintf(mod, "invalid vertex name: %s", name);
		return (NULL);
	}

	if ((vtx = topo_vertex_new(mod, name, inst)) == NULL) {
		topo_mod_dprintf(mod, "failed to create vertex: "
		    "%s=%" PRIx64 "", name, inst);
		return (NULL);
	}
	tn = topo_vertex_node(vtx);

	if (topo_pgroup_create(tn, &pgi, &err) != 0) {
		topo_mod_dprintf(mod, "failed to create %s propgroup on "
		    "%s=%" PRIx64 ": %s", pgi.tpi_name, name, inst,
		    topo_strerror(err));
	}
	return (vtx);
}

uint64_t
wwn_to_uint64(HBA_WWN wwn)
{
	uint64_t res;
	(void) memcpy(&res, &wwn, sizeof(uint64_t));
	return (ntohll(res));
}

typedef struct sas_node {
	SMHBA_PORTATTRIBUTES attrs;
	SMHBA_SAS_PORT sas_attr;
	uint64_t local_wwn;
	uint64_t att_wwn;
} sas_node_t;

int
process_ports(topo_mod_t *mod, HBA_HANDLE *handle, uint32_t port,
    uint32_t num_disc_ports, topo_vertex_t *upstream, uint64_t upstream_sasaddr)
{
	int ret = 0;
	int i;

	topo_mod_dprintf(mod, "process_ports\n");

	for (i = 0; i < num_disc_ports; i++) {
		sas_node_t *device_node;
		SMHBA_SAS_PORT *sas_port;
		const char *vertex_type;
		topo_vertex_t *down_port_vertex, *att_port_vertex, *device_vertex;
		uint64_t att_wwn, local_wwn;

		device_node = topo_mod_zalloc(mod, sizeof(sas_node_t));
		device_node->attrs.PortSpecificAttribute.SASPort =
		    &device_node->sas_attr;

		if ((ret = SMHBA_GetDiscoveredPortAttributes(*handle, port, i,
		    &device_node->attrs)) != HBA_STATUS_OK) {
			topo_mod_dprintf(mod, "failed to get disc port attrs"
			    "for port %d:%d (%d)\n", port, i, ret);
			goto done;
		}

		/* XXX skip these? */
		if (device_node->attrs.PortState != HBA_PORTSTATE_ONLINE) {
			topo_mod_dprintf(mod, "Port %d not online\n", i);
			continue;
		}

		sas_port = device_node->attrs.PortSpecificAttribute.SASPort;
		local_wwn = wwn_to_uint64(sas_port->LocalSASAddress);
		att_wwn = wwn_to_uint64(sas_port->AttachedSASAddress);

		if (att_wwn == upstream_sasaddr) {
			switch (device_node->attrs.PortType) {
			case HBA_PORTTYPE_SASEXPANDER:
				vertex_type = TOPO_VTX_EXPANDER;
				break;
			case HBA_PORTTYPE_SATADEVICE:
			case HBA_PORTTYPE_SASDEVICE:
				vertex_type = TOPO_VTX_TARGET;
				break;
			default:
				topo_mod_dprintf(mod, "unrecognized port type:"
				    "%d\n", device_node->attrs.PortType);
				ret = -1;
				topo_mod_free(mod, device_node,
				    sizeof(sas_node_t));
				goto done;
			}

			att_port_vertex = sas_create_vertex(mod, TOPO_VTX_PORT,
			    att_wwn);
			topo_edge_new(mod, upstream, att_port_vertex);

			if ((down_port_vertex = sas_create_vertex(mod, TOPO_VTX_PORT,
			    local_wwn)) == NULL) {
				ret = -1;
				topo_mod_free(mod, device_node,
				    sizeof(sas_node_t));
				goto done;
			}

			if ((device_vertex = sas_create_vertex(mod, vertex_type,
			    local_wwn)) == NULL) {
				ret = -1;
				topo_mod_free(mod, device_node,
				    sizeof(sas_node_t));
				goto done;
			}

			/* Store the node's data */
			topo_node_setspecific(device_vertex->tvt_node,
			    device_node);

			if (topo_edge_new(mod, att_port_vertex, down_port_vertex) != 0 ||
			    topo_edge_new(mod, down_port_vertex,
			    device_vertex) != 0) {

				ret = -1;
				goto done;
			}

			if (device_node->attrs.PortType ==
			    HBA_PORTTYPE_SASEXPANDER) {
				if ((ret = process_ports(mod, handle, port,
				    num_disc_ports, device_vertex,
				    local_wwn)) != HBA_STATUS_OK) {

					ret = -1;
					topo_mod_free(mod, device_node,
					    sizeof(sas_node_t));
					goto done;
				}
			}
		}
	}

done:
	return ret;
}

int
process_hba(topo_mod_t *mod, uint_t hba_index)
{
	HBA_STATUS status;
	HBA_HANDLE handle;
	SMHBA_ADAPTERATTRIBUTES attrs;
	HBA_UINT32 num_ports;
	int i;
	int ret = 0;

	char aname[256];
	topo_vertex_t *hba;

	if ((status = HBA_GetAdapterName(hba_index, aname)) != 0) {
		ret = -1;
		goto done;
	}
	topo_mod_dprintf(mod, "adapter name: %s\n", aname);

	if (aname[0] == '\0') {
		topo_mod_dprintf(mod, "invalid adapter name\n");
		ret = -1;
		goto done;
	}

	if ((handle = HBA_OpenAdapter(aname)) == 0) {
		topo_mod_dprintf(mod, "couldn't open adapter '%s'\n", aname);
		ret = -1;
		goto done;
	}

	if ((status = SMHBA_GetAdapterAttributes(handle, &attrs)) !=
	    HBA_STATUS_OK) {
		topo_mod_dprintf(mod, "couldn't get adapter attributes"
		    " for '%s'\n", aname);
		ret = -1;
		goto done;
	}

	if ((status = SMHBA_GetNumberOfPorts(handle, &num_ports)) !=
	    HBA_STATUS_OK) {
		topo_mod_dprintf(mod, "couldn't get number of ports"
		    " for '%s'\n", aname);
		ret = -1;
		goto done;
	}

	topo_mod_dprintf(mod, "num ports: %d\n", num_ports);

	for (i = 0; i < num_ports; i++) {
		SMHBA_PORTATTRIBUTES *port_attrs;
		SMHBA_SAS_PORT *sas_port;
		uint64_t local_wwn, att_wwn;
		HBA_UINT32 num_disc_ports;

		sas_node_t *hba_node;
		hba_node = topo_mod_zalloc(mod, sizeof(sas_node_t));
		hba_node->attrs.PortSpecificAttribute.SASPort =
		    &hba_node->sas_attr;
		
		port_attrs = &hba_node->attrs;
		sas_port = &hba_node->sas_attr;

		topo_mod_dprintf(mod, "processing port: %d\n", i);

		if ((status = SMHBA_GetAdapterPortAttributes(handle, i,
		    port_attrs)) != HBA_STATUS_OK) {

			topo_mod_dprintf(mod, "couldn't get port attrs for"
				" '%s' port %d (%d)\n", aname, i, status);
			ret = -1;
			topo_mod_free(mod, hba_node, sizeof(sas_node_t));
			goto done;
		}

		/* XXX make sure this is a SAS port, not FC. */
		topo_mod_dprintf(mod, "type: %d\n", port_attrs->PortType);

		local_wwn = wwn_to_uint64(sas_port->LocalSASAddress);
		att_wwn = wwn_to_uint64(sas_port->AttachedSASAddress);

		/* XXX we still want to add this to topo, right? */
		/*
		if (port_attrs->PortState != HBA_PORTSTATE_ONLINE) {
			continue;
		}
		*/

		if ((hba = sas_create_vertex(mod,
		    TOPO_VTX_INITIATOR, local_wwn)) == NULL) {
			ret = -1;
			topo_mod_free(mod, hba_node, sizeof(sas_node_t));
			goto done;
		}

		/* XXX unique per HBA or per port? */
		hba_node->local_wwn = local_wwn;
		hba_node->att_wwn = att_wwn;

		num_disc_ports = sas_port->NumberofDiscoveredPorts;
		ret = process_ports(mod, &handle, i, num_disc_ports, hba,
		    local_wwn);
		if (ret != 0) {
			topo_mod_free(mod, hba_node, sizeof(sas_node_t));
			goto done;
		}
	}

done:
	/* XXX free hba sas_node_t */
	HBA_CloseAdapter(handle);
	topo_mod_dprintf(mod, "done processing hba '%s'\n", aname);
	return (ret);
}

/* ARGSUSED */
static int
fake_enum(topo_mod_t *mod, tnode_t *rnode, const char *name,
    topo_instance_t min, topo_instance_t max, void *notused1, void *notused2)
{
	/*
	 * XXX - this code simply hardcodes a minimal topology in order to
	 * facilitate early unit testing of the topo_digraph code.  This
	 * will be replaced by proper code that will discover and dynamically
	 * enumerate the SAS fabric(s).
	 */
	topo_vertex_t *ini, *ini_p1, *exp_in1, *exp, *exp_out1, *exp_out2,
	    *tgt1_p1, *tgt2_p1, *tgt1, *tgt2;

	topo_vertex_t *exp_out3, *tgt3_p1, *tgt3, *exp2, *exp2_in1, *exp2_out1;

	uint64_t ini_addr = 0x5003048023567a00;
	uint64_t exp_addr = 0x500304801861347f;
	uint64_t tg1_addr = 0x5000cca2531b1025;
	uint64_t tg2_addr = 0x5000cca2531a41b9;

	uint64_t tg3_addr = 0xDEADBEED;
	uint64_t exp2_addr = 0xDEADBEEF;

	if ((ini = sas_create_vertex(mod, TOPO_VTX_INITIATOR, ini_addr)) ==
	    NULL)
		return (-1);
	if ((ini_p1 = sas_create_vertex(mod, TOPO_VTX_PORT, ini_addr)) == NULL)
		return (-1);
	if (topo_edge_new(mod, ini, ini_p1) != 0)
		return (-1);

	if ((exp_in1 = sas_create_vertex(mod, TOPO_VTX_PORT, ini_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, ini_p1, exp_in1) != 0)
		return (-1);

	if ((exp = sas_create_vertex(mod, TOPO_VTX_EXPANDER, exp_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp_in1, exp) != 0)
		return (-1);

	if ((exp_out1 = sas_create_vertex(mod, TOPO_VTX_PORT, tg1_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp, exp_out1) != 0)
		return (-1);

	if ((tgt1_p1 = sas_create_vertex(mod, TOPO_VTX_PORT, tg1_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp_out1, tgt1_p1) != 0)
		return (-1);

	if ((tgt1 = sas_create_vertex(mod, TOPO_VTX_TARGET, tg1_addr)) == NULL)
		return (-1);
	if (topo_edge_new(mod, tgt1_p1, tgt1) != 0)
		return (-1);

	if ((exp_out2 = sas_create_vertex(mod, TOPO_VTX_PORT, tg2_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp, exp_out2) != 0)
		return (-1);

	if ((tgt2_p1 = sas_create_vertex(mod, TOPO_VTX_PORT, tg2_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp_out2, tgt2_p1) != 0)
		return (-1);

	if ((tgt2 = sas_create_vertex(mod, TOPO_VTX_TARGET, tg2_addr)) == NULL)
		return (-1);
	if (topo_edge_new(mod, tgt2_p1, tgt2) != 0)
		return (-1);

	/* Attach to second expander with one target device */
	if ((exp_out3 = sas_create_vertex(mod, TOPO_VTX_PORT, exp_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp, exp_out3) != 0)
		return (-1);

	if ((exp2_in1 = sas_create_vertex(mod, TOPO_VTX_PORT, exp2_addr)) ==
	    NULL) {
		return (-1);
	}
	if (topo_edge_new(mod, exp_out3, exp2_in1) != 0)
		return (-1);

	if ((exp2 = sas_create_vertex(mod, TOPO_VTX_EXPANDER, exp2_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp2_in1, exp2) != 0)
		return (-1);

	if ((exp2_out1 = sas_create_vertex(mod, TOPO_VTX_PORT, tg3_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp2, exp2_out1) != 0)
		return (-1);

	if ((tgt3_p1 = sas_create_vertex(mod, TOPO_VTX_PORT, tg3_addr)) ==
	    NULL)
		return (-1);
	if (topo_edge_new(mod, exp2_out1, tgt3_p1) != 0)
		return (-1);

	if ((tgt3 = sas_create_vertex(mod, TOPO_VTX_TARGET, tg3_addr)) == NULL)
		return (-1);
	if (topo_edge_new(mod, tgt3_p1, tgt3) != 0)
		return (-1);

	return (0);
}

/*ARGSUSED*/
static int
sas_enum(topo_mod_t *mod, tnode_t *rnode, const char *name,
    topo_instance_t min, topo_instance_t max, void *notused1, void *notused2)
{
	if (topo_method_register(mod, rnode, sas_methods) != 0) {
		topo_mod_dprintf(mod, "failed to register scheme methods");
		/* errno set */
		return (-1);
	}

	if (B_FALSE)
		fake_enum(mod, rnode, name, min, max, notused1, notused2);

	HBA_STATUS status;
	HBA_UINT32 num_adapters;
	uint_t i;

	if ((status = HBA_LoadLibrary()) != HBA_STATUS_OK) {
		return (-1);
	}

	num_adapters = HBA_GetNumberOfAdapters();
	if (num_adapters == 0) {
		// XXX still try to iterate the stuff in /dev/smp/ ?
		return (-1);
	}

	for (i = 0; i < num_adapters; i++) {
		process_hba(mod, i);
	}

	HBA_FreeLibrary();
	return (0);
}

static void
sas_release(topo_mod_t *mod, tnode_t *node)
{
	topo_method_unregister_all(mod, node);
}

/*
 * XXX still need to implement the two methods below
 */

/*
 * This is a prop method that returns the dev-scheme FMRI of the component.
 * This should be registered on the underlying nodes for initiator, expander
 * and target vertices.
 */
static int
sas_dev_fmri(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	if (version > TOPO_METH_FMRI_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	return (-1);
}

/*
 * This is a prop method that returns the hc-scheme FMRI of the corresponding
 * component in the hc-scheme topology.  This should be registered on the
 * underlying nodes for initiator and non-SMP target vertices.
 *
 * For initiators this would be the corresponding pciexfn node.
 * For disk/ssd targets, this would be thew corresponding disk node.  For SES
 * targets, this would be the corresonding ses-enclosure node.  SMP targets
 * are not represented in the hc-scheme topology.
 */
static int
sas_hc_fmri(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	if (version > TOPO_METH_FMRI_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	return (-1);
}

static ssize_t
fmri_bufsz(nvlist_t *nvl)
{
	nvlist_t **paths;
	uint_t nelem;
	ssize_t bufsz = 0;

	bufsz += snprintf(NULL, 0, "sas://");
	if (nvlist_lookup_nvlist_array(nvl, FM_FMRI_SAS_PATH, &paths,
	    &nelem) != 0) {
		return (0);
	}

	for (uint_t i = 0; i < nelem; i++) {
		char *sasname;
		uint64_t sasaddr;

		if (nvlist_lookup_string(paths[i], FM_FMRI_SAS_NAME,
		    &sasname) != 0 ||
		    nvlist_lookup_uint64(paths[i], FM_FMRI_SAS_ADDR,
		    &sasaddr) != 0) {
			return (0);
		}
		bufsz += snprintf(NULL, 0, "/%s=%" PRIx64 "", sasname,
		    sasaddr);
	}
	return (bufsz);
}

/*ARGSUSED*/
static int
sas_fmri_nvl2str(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	uint8_t scheme_vers;
	nvlist_t *outnvl;
	nvlist_t **paths;
	uint_t nelem;
	ssize_t bufsz, end = 0;
	char *buf;

	if (version > TOPO_METH_NVL2STR_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_uint8(in, FM_FMRI_SAS_VERSION, &scheme_vers) != 0 ||
	    scheme_vers != FM_SAS_SCHEME_VERSION) {
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}

	/*
	 * Get size of buffer needed to hold the string representation of the
	 * FMRI.
	 */
	if ((bufsz = fmri_bufsz(in)) == 0) {
		return (topo_mod_seterrno(mod, EMOD_FMRI_MALFORM));
	}

	if ((buf = topo_mod_zalloc(mod, bufsz)) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}

	/*
	 * We've already successfully done these nvlist lookups in fmri_bufsz()
	 * so we don't worry about checking retvals this time around.
	 */
	end += sprintf(buf, "sas://");
	(void) nvlist_lookup_nvlist_array(in, FM_FMRI_SAS_PATH, &paths,
	    &nelem);
	for (uint_t i = 0; i < nelem; i++) {
		char *sasname;
		uint64_t sasaddr;

		(void) nvlist_lookup_string(paths[i], FM_FMRI_SAS_NAME,
		    &sasname);
		(void) nvlist_lookup_uint64(paths[i], FM_FMRI_SAS_ADDR,
		    &sasaddr);
		end += sprintf(buf + end, "/%s=%" PRIx64 "", sasname,
		    sasaddr);
	}

	if (topo_mod_nvalloc(mod, &outnvl, NV_UNIQUE_NAME) != 0) {
		topo_mod_free(mod, buf, bufsz);
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}
	if (nvlist_add_string(outnvl, "fmri-string", buf) != 0) {
		nvlist_free(outnvl);
		topo_mod_free(mod, buf, bufsz);
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}
	topo_mod_free(mod, buf, bufsz);
	*out = outnvl;

	return (0);
}

/*ARGSUSED*/
static int
sas_fmri_str2nvl(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	char *fmristr, *tmp, *lastpair;
	char *sasname;
	nvlist_t *fmri = NULL, **sas_path = NULL;
	uint_t npairs = 0, i = 0, fmrilen;

	if (version > TOPO_METH_STR2NVL_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_string(in, "fmri-string", &fmristr) != 0)
		return (topo_mod_seterrno(mod, EMOD_METHOD_INVAL));

	if (strncmp(fmristr, "sas:///", 7) != 0)
		return (topo_mod_seterrno(mod, EMOD_FMRI_MALFORM));

	if (topo_mod_nvalloc(mod, &fmri, NV_UNIQUE_NAME) != 0) {
		/* errno set */
		return (-1);
	}
	if (nvlist_add_string(fmri, FM_FMRI_SCHEME,
	    FM_FMRI_SCHEME_SAS) != 0 ||
	    nvlist_add_uint8(fmri, FM_FMRI_SAS_VERSION,
	    FM_SAS_SCHEME_VERSION) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	/*
	 * Count the number of "=" chars after the "sas:///" portion of the
	 * FMRI to determine how big the sas-path array needs to be.
	 *
	 * We need to make a copy of the fmri string because strtok will
	 * modify it.  We can't use topo_mod_strdup/strfree because
	 * topo_mod_strfree will end up leaking part of the string because
	 * of the NUL chars that strtok inserts - which will cause
	 * topo_mod_strfree to miscalculate the length of the string.  So we
	 * keep track of the length of the original string and use
	 * topo_mod_zalloc/topo_mod_free.
	 */
	fmrilen = strlen(fmristr);
	if ((tmp = topo_mod_zalloc(mod, fmrilen + 1)) == NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	(void) strncpy(tmp, fmristr, fmrilen);
	(void) strtok_r(tmp + 7, "=", &lastpair);
	while (strtok_r(NULL, "=", &lastpair) != NULL)
		npairs++;

	topo_mod_free(mod, tmp, fmrilen + 1);

	if ((sas_path = topo_mod_zalloc(mod, npairs + sizeof (nvlist_t *))) ==
	    NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	sasname = fmristr + 7;
	while (i < npairs) {
		nvlist_t *pathcomp;
		uint64_t sasaddr;
		char *end, *addrstr, *estr;

		if (topo_mod_nvalloc(mod, &pathcomp, NV_UNIQUE_NAME) != 0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			goto err;
		}
		if ((end = strchr(sasname, '=')) == NULL) {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}
		*end = '\0';

		addrstr = end + 1;

		/*
		 * If this is the last pair, then addrstr will already be
		 * nul-terminated.
		 */
		if (i < (npairs - 1)) {
			if ((end = strchr(addrstr, '/')) == NULL) {
				(void) topo_mod_seterrno(mod,
				    EMOD_FMRI_MALFORM);
				goto err;
			}
			*end = '\0';
		}

		/*
		 * Convert addrstr to a uint64_t
		 */
		errno = 0;
		sasaddr = strtoull(addrstr, &estr, 16);
		if (errno != 0 || *estr != '\0') {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}

		/*
		 * Add both nvpairs to the nvlist and then add the nvlist to
		 * the sas-path nvlist array.
		 */
		if (nvlist_add_string(pathcomp, FM_FMRI_SAS_NAME, sasname) !=
		    0 ||
		    nvlist_add_uint64(pathcomp, FM_FMRI_SAS_ADDR, sasaddr) !=
		    0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			goto err;
		}
		sas_path[i++] = pathcomp;
		sasname = end + 1;
	}
	if (nvlist_add_nvlist_array(fmri, FM_FMRI_SAS_PATH, sas_path,
	    npairs) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = fmri;

	return (0);
err:
	topo_mod_dprintf(mod, "sas_fmri_str2nvl failed: %s",
	    topo_strerror(topo_mod_errno(mod)));
	if (sas_path != NULL) {
		for (i = 0; i < npairs; i++)
			nvlist_free(sas_path[i]);

		topo_mod_free(mod, sas_path, npairs + sizeof (nvlist_t *));
	}
	nvlist_free(fmri);
	return (-1);
}
