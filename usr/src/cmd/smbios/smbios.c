/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2015 OmniTI Computer Consulting, Inc.  All rights reserved.
 * Copyright (c) 2018, Joyent, Inc.
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/sysmacros.h>
#include <sys/param.h>

#include <smbios.h>
#include <alloca.h>
#include <limits.h>
#include <unistd.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <libjedec.h>

#define	SMBIOS_SUCCESS	0
#define	SMBIOS_ERROR	1
#define	SMBIOS_USAGE	2

static const char *g_pname;
static int g_hdr;

static int opt_e;
static int opt_i = -1;
static int opt_O;
static int opt_s;
static int opt_t = -1;
static int opt_x;

static boolean_t
smbios_vergteq(smbios_version_t *v, uint_t major, uint_t minor)
{
	if (v->smbv_major > major)
		return (B_TRUE);
	if (v->smbv_major == major &&
	    v->smbv_minor >= minor)
		return (B_TRUE);
	return (B_FALSE);
}

/*PRINTFLIKE2*/
static void
smbios_warn(smbios_hdl_t *shp, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(stderr, format, ap);
	va_end(ap);

	if (shp != NULL) {
		(void) fprintf(stderr, ": %s",
		    smbios_errmsg(smbios_errno(shp)));
	}

	(void) fprintf(stderr, "\n");
}

/*PRINTFLIKE2*/
static void
oprintf(FILE *fp, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(fp, format, ap);
	va_end(ap);
}

/*PRINTFLIKE3*/
static void
desc_printf(const char *d, FILE *fp, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(fp, format, ap);
	va_end(ap);

	if (d != NULL)
		(void) fprintf(fp, " (%s)\n", d);
	else
		(void) fprintf(fp, "\n");
}

static void
flag_printf(FILE *fp, const char *s, uint_t flags, size_t bits,
    const char *(*flag_name)(uint_t), const char *(*flag_desc)(uint_t))
{
	size_t i;

	oprintf(fp, "  %s: 0x%x\n", s, flags);

	for (i = 0; i < bits; i++) {
		uint_t f = 1 << i;
		const char *n;

		if (!(flags & f))
			continue;

		if ((n = flag_name(f)) != NULL)
			desc_printf(flag_desc(f), fp, "\t%s", n);
		else
			desc_printf(flag_desc(f), fp, "\t0x%x", f);
	}
}

static void
flag64_printf(FILE *fp, const char *s, uint64_t flags, size_t bits,
    const char *(*flag_name)(uint64_t), const char *(*flag_desc)(uint64_t))
{
	size_t i;

	oprintf(fp, "  %s: 0x%llx\n", s, (u_longlong_t)flags);

	for (i = 0; i < bits; i++) {
		u_longlong_t f = 1ULL << i;
		const char *n;

		if (!(flags & f))
			continue;

		if ((n = flag_name(f)) != NULL)
			desc_printf(flag_desc(f), fp, "\t%s", n);
		else
			desc_printf(flag_desc(f), fp, "\t0x%llx", f);
	}
}

static void
id_printf(FILE *fp, const char *s, id_t id)
{
	switch (id) {
	case SMB_ID_NONE:
		oprintf(fp, "%sNone\n", s);
		break;
	case SMB_ID_NOTSUP:
		oprintf(fp, "%sNot Supported\n", s);
		break;
	default:
		oprintf(fp, "%s%u\n", s, (uint_t)id);
	}
}

static void
jedec_print(FILE *fp, const char *desc, uint_t id)
{
	const char *name;
	uint_t cont, vendor;

	vendor = id & 0xff;
	cont = (id >> 8) & 0xff;
	name = libjedec_vendor_string(cont, vendor);
	if (name == NULL) {
		oprintf(fp, "  %s: 0x%x\n", desc, id);
	} else {
		oprintf(fp, "  %s: 0x%x (%s)\n", desc, id, name);
	}
}

/*
 * Print a 128-bit data as a series of 16 hex digits.
 */
static void
u128_print(FILE *fp, const char *desc, const uint8_t *data)
{
	uint_t i;

	oprintf(fp, "%s: ", desc);
	for (i = 0; i < 16; i++) {
		oprintf(fp, " %02x", data[i]);
	}
	oprintf(fp, "\n");
}

static int
check_oem(smbios_hdl_t *shp)
{
	int i;
	int cnt;
	int rv;
	id_t oem_id;
	smbios_struct_t s;
	const char **oem_str;

	rv = smbios_lookup_type(shp, SMB_TYPE_OEMSTR, &s);
	if (rv != 0) {
		return (-1);
	}

	oem_id = s.smbstr_id;

	cnt = smbios_info_strtab(shp, oem_id, 0, NULL);
	if (cnt > 0) {
		oem_str =  alloca(sizeof (char *) * cnt);
		(void) smbios_info_strtab(shp, oem_id, cnt, oem_str);

		for (i = 0; i < cnt; i++) {
			if (strncmp(oem_str[i], SMB_PRMS1,
			    strlen(SMB_PRMS1) + 1) == 0) {
				return (0);
			}
		}
	}

	return (-1);
}

static void
print_smbios_21(smbios_21_entry_t *ep, FILE *fp)
{
	int i;

	oprintf(fp, "Entry Point Anchor Tag: %*.*s\n",
	    (int)sizeof (ep->smbe_eanchor), (int)sizeof (ep->smbe_eanchor),
	    ep->smbe_eanchor);

	oprintf(fp, "Entry Point Checksum: 0x%x\n", ep->smbe_ecksum);
	oprintf(fp, "Entry Point Length: %u\n", ep->smbe_elen);
	oprintf(fp, "Entry Point Version: %u.%u\n",
	    ep->smbe_major, ep->smbe_minor);
	oprintf(fp, "Max Structure Size: %u\n", ep->smbe_maxssize);
	oprintf(fp, "Entry Point Revision: 0x%x\n", ep->smbe_revision);

	oprintf(fp, "Entry Point Revision Data:");
	for (i = 0; i < sizeof (ep->smbe_format); i++)
		oprintf(fp, " 0x%02x", ep->smbe_format[i]);
	oprintf(fp, "\n");

	oprintf(fp, "Intermediate Anchor Tag: %*.*s\n",
	    (int)sizeof (ep->smbe_ianchor), (int)sizeof (ep->smbe_ianchor),
	    ep->smbe_ianchor);

	oprintf(fp, "Intermediate Checksum: 0x%x\n", ep->smbe_icksum);
	oprintf(fp, "Structure Table Length: %u\n", ep->smbe_stlen);
	oprintf(fp, "Structure Table Address: 0x%x\n", ep->smbe_staddr);
	oprintf(fp, "Structure Table Entries: %u\n", ep->smbe_stnum);
	oprintf(fp, "DMI BCD Revision: 0x%x\n", ep->smbe_bcdrev);
}

static void
print_smbios_30(smbios_30_entry_t *ep, FILE *fp)
{
	oprintf(fp, "Entry Point Anchor Tag: %*.*s\n",
	    (int)sizeof (ep->smbe_eanchor), (int)sizeof (ep->smbe_eanchor),
	    ep->smbe_eanchor);

	oprintf(fp, "Entry Point Checksum: 0x%x\n", ep->smbe_ecksum);
	oprintf(fp, "Entry Point Length: %u\n", ep->smbe_elen);
	oprintf(fp, "SMBIOS Version: %u.%u\n",
	    ep->smbe_major, ep->smbe_minor);
	oprintf(fp, "SMBIOS DocRev: 0x%x\n", ep->smbe_docrev);
	oprintf(fp, "Entry Point Revision: 0x%x\n", ep->smbe_revision);

	oprintf(fp, "Structure Table Length: %u\n", ep->smbe_stlen);
	oprintf(fp, "Structure Table Address: 0x%" PRIx64 "\n",
	    ep->smbe_staddr);
}

static void
print_smbios(smbios_hdl_t *shp, FILE *fp)
{
	smbios_entry_t ep;

	switch (smbios_info_smbios(shp, &ep)) {
	case SMBIOS_ENTRY_POINT_21:
		print_smbios_21(&ep.ep21, fp);
		break;
	case SMBIOS_ENTRY_POINT_30:
		print_smbios_30(&ep.ep30, fp);
		break;
	}
}

static void
print_common(const smbios_info_t *ip, FILE *fp)
{
	if (ip->smbi_manufacturer[0] != '\0')
		oprintf(fp, "  Manufacturer: %s\n", ip->smbi_manufacturer);
	if (ip->smbi_product[0] != '\0')
		oprintf(fp, "  Product: %s\n", ip->smbi_product);
	if (ip->smbi_version[0] != '\0')
		oprintf(fp, "  Version: %s\n", ip->smbi_version);
	if (ip->smbi_serial[0] != '\0')
		oprintf(fp, "  Serial Number: %s\n", ip->smbi_serial);
	if (ip->smbi_asset[0] != '\0')
		oprintf(fp, "  Asset Tag: %s\n", ip->smbi_asset);
	if (ip->smbi_location[0] != '\0')
		oprintf(fp, "  Location Tag: %s\n", ip->smbi_location);
	if (ip->smbi_part[0] != '\0')
		oprintf(fp, "  Part Number: %s\n", ip->smbi_part);
}

static void
print_bios(smbios_hdl_t *shp, FILE *fp)
{
	smbios_bios_t b;

	(void) smbios_info_bios(shp, &b);

	oprintf(fp, "  Vendor: %s\n", b.smbb_vendor);
	oprintf(fp, "  Version String: %s\n", b.smbb_version);
	oprintf(fp, "  Release Date: %s\n", b.smbb_reldate);
	oprintf(fp, "  Address Segment: 0x%x\n", b.smbb_segment);
	oprintf(fp, "  ROM Size: %" PRIu64 " bytes\n", b.smbb_extromsize);
	oprintf(fp, "  Image Size: %u bytes\n", b.smbb_runsize);

	flag64_printf(fp, "Characteristics",
	    b.smbb_cflags, sizeof (b.smbb_cflags) * NBBY,
	    smbios_bios_flag_name, smbios_bios_flag_desc);

	if (b.smbb_nxcflags > SMB_BIOSXB_1) {
		flag_printf(fp, "Characteristics Extension Byte 1",
		    b.smbb_xcflags[SMB_BIOSXB_1],
		    sizeof (b.smbb_xcflags[SMB_BIOSXB_1]) * NBBY,
		    smbios_bios_xb1_name, smbios_bios_xb1_desc);
	}

	if (b.smbb_nxcflags > SMB_BIOSXB_2) {
		flag_printf(fp, "Characteristics Extension Byte 2",
		    b.smbb_xcflags[SMB_BIOSXB_2],
		    sizeof (b.smbb_xcflags[SMB_BIOSXB_2]) * NBBY,
		    smbios_bios_xb2_name, smbios_bios_xb2_desc);
	}

	if (b.smbb_nxcflags > SMB_BIOSXB_BIOS_MIN) {
		oprintf(fp, "  Version Number: %u.%u\n",
		    b.smbb_biosv.smbv_major, b.smbb_biosv.smbv_minor);
	}

	/*
	 * If the major and minor versions are 0xff then that indicates that the
	 * embedded controller does not exist.
	 */
	if (b.smbb_nxcflags > SMB_BIOSXB_ECFW_MIN &&
	    b.smbb_ecfwv.smbv_major != 0xff &&
	    b.smbb_ecfwv.smbv_minor != 0xff) {
		oprintf(fp, "  Embedded Ctlr Firmware Version Number: %u.%u\n",
		    b.smbb_ecfwv.smbv_major, b.smbb_ecfwv.smbv_minor);
	}
}

static void
print_system(smbios_hdl_t *shp, FILE *fp)
{
	smbios_system_t s;
	uint_t i;

	(void) smbios_info_system(shp, &s);

	/*
	 * SMBIOS definition section 3.3.2.1 is clear that the first three
	 * fields are little-endian, but this utility traditionally got this
	 * wrong, and followed RFC 4122.  We keep this old behavior, but also
	 * provide a corrected UUID.
	 */
	oprintf(fp, "  UUID: ");
	oprintf(fp, "%02x%02x%02x%02x-%02x%02x-%02x%02x-",
	    s.smbs_uuid[0], s.smbs_uuid[1], s.smbs_uuid[2], s.smbs_uuid[3],
	    s.smbs_uuid[4], s.smbs_uuid[5], s.smbs_uuid[6], s.smbs_uuid[7]);
	for (i = 8; i < s.smbs_uuidlen; i++) {
		oprintf(fp, "%02x", s.smbs_uuid[i]);
		if (i == 9)
			oprintf(fp, "-");
	}
	oprintf(fp, "\n");

	oprintf(fp, "  UUID (Endian-corrected): ");
	oprintf(fp, "%08x-%04hx-%04hx-", *((uint_t *)&s.smbs_uuid[0]),
	    *((ushort_t *)&s.smbs_uuid[4]),
	    *((ushort_t *)&s.smbs_uuid[6]));
	for (i = 8; i < s.smbs_uuidlen; i++) {
		oprintf(fp, "%02x", s.smbs_uuid[i]);
		if (i == 9)
			oprintf(fp, "-");
	}
	oprintf(fp, "\n");

	desc_printf(smbios_system_wakeup_desc(s.smbs_wakeup),
	    fp, "  Wake-Up Event: 0x%x", s.smbs_wakeup);

	oprintf(fp, "  SKU Number: %s\n", s.smbs_sku);
	oprintf(fp, "  Family: %s\n", s.smbs_family);
}

static void
print_bboard(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_bboard_t b;
	int chdl_cnt;

	(void) smbios_info_bboard(shp, id, &b);

	oprintf(fp, "  Chassis: %u\n", (uint_t)b.smbb_chassis);

	flag_printf(fp, "Flags", b.smbb_flags, sizeof (b.smbb_flags) * NBBY,
	    smbios_bboard_flag_name, smbios_bboard_flag_desc);

	desc_printf(smbios_bboard_type_desc(b.smbb_type),
	    fp, "  Board Type: 0x%x", b.smbb_type);

	chdl_cnt = b.smbb_contn;
	if (chdl_cnt != 0) {
		id_t *chdl;
		uint16_t hdl;
		int i, n, cnt;

		chdl = alloca(chdl_cnt * sizeof (id_t));
		cnt = smbios_info_contains(shp, id, chdl_cnt, chdl);
		if (cnt > SMB_CONT_MAX)
			return;
		n = MIN(chdl_cnt, cnt);

		oprintf(fp, "\n");
		for (i = 0; i < n; i++) {
			hdl = (uint16_t)chdl[i];
			oprintf(fp, "  Contained Handle: %u\n", hdl);
		}
	}
}

static void
print_chassis(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_chassis_t c;
	int elem_cnt;

	(void) smbios_info_chassis(shp, id, &c);

	oprintf(fp, "  OEM Data: 0x%x\n", c.smbc_oemdata);
	oprintf(fp, "  SKU number: %s\n",
	    c.smbc_sku[0] == '\0' ? "<unknown>" : c.smbc_sku);
	oprintf(fp, "  Lock Present: %s\n", c.smbc_lock ? "Y" : "N");

	desc_printf(smbios_chassis_type_desc(c.smbc_type),
	    fp, "  Chassis Type: 0x%x", c.smbc_type);

	desc_printf(smbios_chassis_state_desc(c.smbc_bustate),
	    fp, "  Boot-Up State: 0x%x", c.smbc_bustate);

	desc_printf(smbios_chassis_state_desc(c.smbc_psstate),
	    fp, "  Power Supply State: 0x%x", c.smbc_psstate);

	desc_printf(smbios_chassis_state_desc(c.smbc_thstate),
	    fp, "  Thermal State: 0x%x", c.smbc_thstate);

	oprintf(fp, "  Chassis Height: %uu\n", c.smbc_uheight);
	oprintf(fp, "  Power Cords: %u\n", c.smbc_cords);

	elem_cnt = c.smbc_elems;
	oprintf(fp, "  Element Records: %u\n", elem_cnt);

	if (elem_cnt > 0) {
		id_t *elems;
		uint8_t type;
		int i, n, cnt;

		elems = alloca(c.smbc_elems * sizeof (id_t));
		cnt = smbios_info_contains(shp, id, elem_cnt, elems);
		if (cnt > SMB_CONT_MAX)
			return;
		n = MIN(elem_cnt, cnt);

		oprintf(fp, "\n");
		for (i = 0; i < n; i++) {
			type = (uint8_t)elems[i];
			if (type & 0x80) {
				/* SMBIOS structrure Type */
				desc_printf(smbios_type_name(type & 0x7f), fp,
				    "  Contained SMBIOS structure Type: %u",
				    type & 0x80);
			} else {
				/* SMBIOS Base Board Type */
				desc_printf(smbios_bboard_type_desc(type), fp,
				    "  Contained SMBIOS Base Board Type: 0x%x",
				    type);
			}
		}
	}
}

static void
print_processor(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_processor_t p;
	uint_t status;

	(void) smbios_info_processor(shp, id, &p);
	status = SMB_PRSTATUS_STATUS(p.smbp_status);

	desc_printf(smbios_processor_family_desc(p.smbp_family),
	    fp, "  Family: %u", p.smbp_family);

	oprintf(fp, "  CPUID: 0x%llx\n", (u_longlong_t)p.smbp_cpuid);

	desc_printf(smbios_processor_type_desc(p.smbp_type),
	    fp, "  Type: %u", p.smbp_type);

	desc_printf(smbios_processor_upgrade_desc(p.smbp_upgrade),
	    fp, "  Socket Upgrade: %u", p.smbp_upgrade);

	oprintf(fp, "  Socket Status: %s\n",
	    SMB_PRSTATUS_PRESENT(p.smbp_status) ?
	    "Populated" : "Not Populated");

	desc_printf(smbios_processor_status_desc(status),
	    fp, "  Processor Status: %u", status);

	if (SMB_PRV_LEGACY(p.smbp_voltage)) {
		oprintf(fp, "  Supported Voltages:");
		switch (p.smbp_voltage) {
		case SMB_PRV_5V:
			oprintf(fp, " 5.0V");
			break;
		case SMB_PRV_33V:
			oprintf(fp, " 3.3V");
			break;
		case SMB_PRV_29V:
			oprintf(fp, " 2.9V");
			break;
		}
		oprintf(fp, "\n");
	} else {
		oprintf(fp, "  Supported Voltages: %.1fV\n",
		    (float)SMB_PRV_VOLTAGE(p.smbp_voltage) / 10);
	}

	if (p.smbp_corecount != 0) {
		oprintf(fp, "  Core Count: %u\n", p.smbp_corecount);
	} else {
		oprintf(fp, "  Core Count: Unknown\n");
	}

	if (p.smbp_coresenabled != 0) {
		oprintf(fp, "  Cores Enabled: %u\n", p.smbp_coresenabled);
	} else {
		oprintf(fp, "  Cores Enabled: Unknown\n");
	}

	if (p.smbp_threadcount != 0) {
		oprintf(fp, "  Thread Count: %u\n", p.smbp_threadcount);
	} else {
		oprintf(fp, "  Thread Count: Unknown\n");
	}

	if (p.smbp_cflags) {
		flag_printf(fp, "Processor Characteristics",
		    p.smbp_cflags, sizeof (p.smbp_cflags) * NBBY,
		    smbios_processor_core_flag_name,
		    smbios_processor_core_flag_desc);
	}

	if (p.smbp_clkspeed != 0)
		oprintf(fp, "  External Clock Speed: %uMHz\n", p.smbp_clkspeed);
	else
		oprintf(fp, "  External Clock Speed: Unknown\n");

	if (p.smbp_maxspeed != 0)
		oprintf(fp, "  Maximum Speed: %uMHz\n", p.smbp_maxspeed);
	else
		oprintf(fp, "  Maximum Speed: Unknown\n");

	if (p.smbp_curspeed != 0)
		oprintf(fp, "  Current Speed: %uMHz\n", p.smbp_curspeed);
	else
		oprintf(fp, "  Current Speed: Unknown\n");

	id_printf(fp, "  L1 Cache Handle: ", p.smbp_l1cache);
	id_printf(fp, "  L2 Cache Handle: ", p.smbp_l2cache);
	id_printf(fp, "  L3 Cache Handle: ", p.smbp_l3cache);
}

static void
print_cache(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_cache_t c;

	(void) smbios_info_cache(shp, id, &c);

	oprintf(fp, "  Level: %u\n", c.smba_level);
	oprintf(fp, "  Maximum Installed Size: %" PRIu64 " bytes\n",
	    c.smba_maxsize2);

	if (c.smba_size2 != 0) {
		oprintf(fp, "  Installed Size: %" PRIu64 " bytes\n",
		    c.smba_size2);
	} else {
		oprintf(fp, "  Installed Size: Not Installed\n");
	}

	if (c.smba_speed != 0)
		oprintf(fp, "  Speed: %uns\n", c.smba_speed);
	else
		oprintf(fp, "  Speed: Unknown\n");

	flag_printf(fp, "Supported SRAM Types",
	    c.smba_stype, sizeof (c.smba_stype) * NBBY,
	    smbios_cache_ctype_name, smbios_cache_ctype_desc);

	desc_printf(smbios_cache_ctype_desc(c.smba_ctype),
	    fp, "  Current SRAM Type: 0x%x", c.smba_ctype);

	desc_printf(smbios_cache_ecc_desc(c.smba_etype),
	    fp, "  Error Correction Type: %u", c.smba_etype);

	desc_printf(smbios_cache_logical_desc(c.smba_ltype),
	    fp, "  Logical Cache Type: %u", c.smba_ltype);

	desc_printf(smbios_cache_assoc_desc(c.smba_assoc),
	    fp, "  Associativity: %u", c.smba_assoc);

	desc_printf(smbios_cache_mode_desc(c.smba_mode),
	    fp, "  Mode: %u", c.smba_mode);

	desc_printf(smbios_cache_loc_desc(c.smba_location),
	    fp, "  Location: %u", c.smba_location);

	flag_printf(fp, "Flags", c.smba_flags, sizeof (c.smba_flags) * NBBY,
	    smbios_cache_flag_name, smbios_cache_flag_desc);
}

static void
print_port(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_port_t p;

	(void) smbios_info_port(shp, id, &p);

	oprintf(fp, "  Internal Reference Designator: %s\n", p.smbo_iref);
	oprintf(fp, "  External Reference Designator: %s\n", p.smbo_eref);

	desc_printf(smbios_port_conn_desc(p.smbo_itype),
	    fp, "  Internal Connector Type: %u", p.smbo_itype);

	desc_printf(smbios_port_conn_desc(p.smbo_etype),
	    fp, "  External Connector Type: %u", p.smbo_etype);

	desc_printf(smbios_port_type_desc(p.smbo_ptype),
	    fp, "  Port Type: %u", p.smbo_ptype);
}

static void
print_slot(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_slot_t s;
	smbios_version_t v;

	(void) smbios_info_slot(shp, id, &s);
	smbios_info_smbios_version(shp, &v);

	oprintf(fp, "  Reference Designator: %s\n", s.smbl_name);
	oprintf(fp, "  Slot ID: 0x%x\n", s.smbl_id);

	desc_printf(smbios_slot_type_desc(s.smbl_type),
	    fp, "  Type: 0x%x", s.smbl_type);

	desc_printf(smbios_slot_width_desc(s.smbl_width),
	    fp, "  Width: 0x%x", s.smbl_width);

	desc_printf(smbios_slot_usage_desc(s.smbl_usage),
	    fp, "  Usage: 0x%x", s.smbl_usage);

	desc_printf(smbios_slot_length_desc(s.smbl_length),
	    fp, "  Length: 0x%x", s.smbl_length);

	flag_printf(fp, "Slot Characteristics 1",
	    s.smbl_ch1, sizeof (s.smbl_ch1) * NBBY,
	    smbios_slot_ch1_name, smbios_slot_ch1_desc);

	flag_printf(fp, "Slot Characteristics 2",
	    s.smbl_ch2, sizeof (s.smbl_ch2) * NBBY,
	    smbios_slot_ch2_name, smbios_slot_ch2_desc);

	if (check_oem(shp) != 0 && !smbios_vergteq(&v, 2, 6))
		return;

	oprintf(fp, "  Segment Group: %u\n", s.smbl_sg);
	oprintf(fp, "  Bus Number: %u\n", s.smbl_bus);
	oprintf(fp, "  Device/Function Number: %u/%u\n", s.smbl_df >> 3,
	    s.smbl_df & 0x7);

	if (s.smbl_dbw != 0) {
		oprintf(fp, "  Data Bus Width: %d\n", s.smbl_dbw);
	}

	if (s.smbl_npeers > 0) {
		smbios_slot_peer_t *peer;
		uint_t i, npeers;

		if (smbios_info_slot_peers(shp, id, &npeers, &peer) != 0) {
			smbios_warn(shp, "failed to read slot peer "
			    "information");
			return;
		}

		for (i = 0; i < npeers; i++) {
			oprintf(fp, "  Slot Peer %u:\n", i);
			oprintf(fp, "    Segment group: %u\n",
			    peer[i].smblp_group);
			oprintf(fp, "    Bus/Device/Function: %u/%u/%u",
			    peer[i].smblp_bus, peer[i].smblp_device,
			    peer[i].smblp_function);
			oprintf(fp, "    Electrical width: %u\n",
			    peer[i].smblp_data_width);
		}

		smbios_info_slot_peers_free(shp, npeers, peer);
	}
}

static void
print_obdevs_ext(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	boolean_t enabled;
	smbios_obdev_ext_t oe;
	const char *type;

	(void) smbios_info_obdevs_ext(shp, id, &oe);

	/*
	 * Bit 7 is always whether or not the device is enabled while bits 0:6
	 * are the actual device type.
	 */
	enabled = oe.smboe_dtype >> 7;
	type = smbios_onboard_type_desc(oe.smboe_dtype & 0x7f);

	oprintf(fp, "  Reference Designator: %s\n", oe.smboe_name);
	oprintf(fp, "  Device Enabled: %s\n", enabled == B_TRUE ? "true" :
	    "false");
	oprintf(fp, "  Device Type: %s\n", type);
	oprintf(fp, "  Device Type Instance: %u\n", oe.smboe_dti);
	oprintf(fp, "  Segment Group Number: %u\n", oe.smboe_sg);
	oprintf(fp, "  Bus Number: %u\n", oe.smboe_bus);
	oprintf(fp, "  Device/Function Number: %u\n", oe.smboe_df);
}

static void
print_obdevs(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_obdev_t *argv;
	int i, argc;

	if ((argc = smbios_info_obdevs(shp, id, 0, NULL)) > 0) {
		argv = alloca(sizeof (smbios_obdev_t) * argc);
		(void) smbios_info_obdevs(shp, id, argc, argv);
		for (i = 0; i < argc; i++)
			oprintf(fp, "  %s\n", argv[i].smbd_name);
	}
}

static void
print_strtab(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	const char **argv;
	int i, argc;

	if ((argc = smbios_info_strtab(shp, id, 0, NULL)) > 0) {
		argv = alloca(sizeof (char *) * argc);
		(void) smbios_info_strtab(shp, id, argc, argv);
		for (i = 0; i < argc; i++)
			oprintf(fp, "  %s\n", argv[i]);
	}
}

static void
print_lang(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_lang_t l;

	(void) smbios_info_lang(shp, &l);

	oprintf(fp, "  Current Language: %s\n", l.smbla_cur);
	oprintf(fp, "  Language String Format: %u\n", l.smbla_fmt);
	oprintf(fp, "  Number of Installed Languages: %u\n", l.smbla_num);
	oprintf(fp, "  Installed Languages:\n");

	print_strtab(shp, id, fp);
}

/*ARGSUSED*/
static void
print_evlog(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_evlog_t ev;
	uint32_t i;

	(void) smbios_info_eventlog(shp, &ev);

	oprintf(fp, "  Log Area Size: %lu bytes\n", (ulong_t)ev.smbev_size);
	oprintf(fp, "  Header Offset: %lu\n", (ulong_t)ev.smbev_hdr);
	oprintf(fp, "  Data Offset: %lu\n", (ulong_t)ev.smbev_data);

	desc_printf(smbios_evlog_method_desc(ev.smbev_method),
	    fp, "  Data Access Method: %u", ev.smbev_method);

	flag_printf(fp, "Log Flags",
	    ev.smbev_flags, sizeof (ev.smbev_flags) * NBBY,
	    smbios_evlog_flag_name, smbios_evlog_flag_desc);

	desc_printf(smbios_evlog_format_desc(ev.smbev_format),
	    fp, "  Log Header Format: %u", ev.smbev_format);

	oprintf(fp, "  Update Token: 0x%x\n", ev.smbev_token);
	oprintf(fp, "  Data Access Address: ");

	switch (ev.smbev_method) {
	case SMB_EVM_1x1i_1x1d:
	case SMB_EVM_2x1i_1x1d:
	case SMB_EVM_1x2i_1x1d:
		oprintf(fp, "Index Address 0x%x, Data Address 0x%x\n",
		    ev.smbev_addr.eva_io.evi_iaddr,
		    ev.smbev_addr.eva_io.evi_daddr);
		break;
	case SMB_EVM_GPNV:
		oprintf(fp, "0x%x\n", ev.smbev_addr.eva_gpnv);
		break;
	default:
		oprintf(fp, "0x%x\n", ev.smbev_addr.eva_addr);
	}

	oprintf(fp, "  Type Descriptors:\n");

	for (i = 0; i < ev.smbev_typec; i++) {
		oprintf(fp, "  %u: Log Type 0x%x, Data Type 0x%x\n", i,
		    ev.smbev_typev[i].smbevt_ltype,
		    ev.smbev_typev[i].smbevt_dtype);
	}
}

static void
print_bytes(const uint8_t *data, size_t size, FILE *fp)
{
	size_t row, rows = P2ROUNDUP(size, 16) / 16;
	size_t col, cols;

	char buf[17];
	uint8_t x;

	oprintf(fp, "\n  offset:   0 1 2 3  4 5 6 7  8 9 a b  c d e f  "
	    "0123456789abcdef\n");

	for (row = 0; row < rows; row++) {
		oprintf(fp, "  %#6lx: ", (ulong_t)row * 16);
		cols = MIN(size - row * 16, 16);

		for (col = 0; col < cols; col++) {
			if (col % 4 == 0)
				oprintf(fp, " ");
			x = *data++;
			oprintf(fp, "%02x", x);
			buf[col] = x <= ' ' || x > '~' ? '.' : x;
		}

		for (; col < 16; col++) {
			if (col % 4 == 0)
				oprintf(fp, " ");
			oprintf(fp, "  ");
			buf[col] = ' ';
		}

		buf[col] = '\0';
		oprintf(fp, "  %s\n", buf);
	}

	oprintf(fp, "\n");
}

static void
print_memarray(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_memarray_t ma;

	(void) smbios_info_memarray(shp, id, &ma);

	desc_printf(smbios_memarray_loc_desc(ma.smbma_location),
	    fp, "  Location: %u", ma.smbma_location);

	desc_printf(smbios_memarray_use_desc(ma.smbma_use),
	    fp, "  Use: %u", ma.smbma_use);

	desc_printf(smbios_memarray_ecc_desc(ma.smbma_ecc),
	    fp, "  ECC: %u", ma.smbma_ecc);

	oprintf(fp, "  Number of Slots/Sockets: %u\n", ma.smbma_ndevs);
	id_printf(fp, "  Memory Error Data: ", ma.smbma_err);
	oprintf(fp, "  Max Capacity: %llu bytes\n",
	    (u_longlong_t)ma.smbma_size);
}

static void
print_memdevice(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_memdevice_t md;

	(void) smbios_info_memdevice(shp, id, &md);

	id_printf(fp, "  Physical Memory Array: ", md.smbmd_array);
	id_printf(fp, "  Memory Error Data: ", md.smbmd_error);

	if (md.smbmd_twidth != -1u)
		oprintf(fp, "  Total Width: %u bits\n", md.smbmd_twidth);
	else
		oprintf(fp, "  Total Width: Unknown\n");

	if (md.smbmd_dwidth != -1u)
		oprintf(fp, "  Data Width: %u bits\n", md.smbmd_dwidth);
	else
		oprintf(fp, "  Data Width: Unknown\n");

	switch (md.smbmd_size) {
	case -1ull:
		oprintf(fp, "  Size: Unknown\n");
		break;
	case 0:
		oprintf(fp, "  Size: Not Populated\n");
		break;
	default:
		oprintf(fp, "  Size: %llu bytes\n",
		    (u_longlong_t)md.smbmd_size);
	}

	desc_printf(smbios_memdevice_form_desc(md.smbmd_form),
	    fp, "  Form Factor: %u", md.smbmd_form);

	if (md.smbmd_set == 0)
		oprintf(fp, "  Set: None\n");
	else if (md.smbmd_set == (uint8_t)-1u)
		oprintf(fp, "  Set: Unknown\n");
	else
		oprintf(fp, "  Set: %u\n", md.smbmd_set);

	if (md.smbmd_rank != 0) {
		desc_printf(smbios_memdevice_rank_desc(md.smbmd_rank),
		    fp, "  Rank: %u", md.smbmd_rank);
	} else {
		oprintf(fp, "  Rank: Unknown\n");
	}

	desc_printf(smbios_memdevice_type_desc(md.smbmd_type),
	    fp, "  Memory Type: %u", md.smbmd_type);

	flag_printf(fp, "Flags", md.smbmd_flags, sizeof (md.smbmd_flags) * NBBY,
	    smbios_memdevice_flag_name, smbios_memdevice_flag_desc);

	if (md.smbmd_extspeed != 0) {
		oprintf(fp, "  Speed: %" PRIu64 " MT/s\n", md.smbmd_extspeed);
	} else {
		oprintf(fp, "  Speed: Unknown\n");
	}

	if (md.smbmd_extclkspeed != 0) {
		oprintf(fp, "  Configured Speed: %" PRIu64 " MT/s\n",
		    md.smbmd_extclkspeed);
	} else {
		oprintf(fp, "  Configured Speed: Unknown\n");
	}

	oprintf(fp, "  Device Locator: %s\n", md.smbmd_dloc);
	oprintf(fp, "  Bank Locator: %s\n", md.smbmd_bloc);

	if (md.smbmd_minvolt != 0) {
		oprintf(fp, "  Minimum Voltage: %.2fV\n",
		    md.smbmd_minvolt / 1000.0);
	} else {
		oprintf(fp, "  Minimum Voltage: Unknown\n");
	}

	if (md.smbmd_maxvolt != 0) {
		oprintf(fp, "  Maximum Voltage: %.2fV\n",
		    md.smbmd_maxvolt / 1000.0);
	} else {
		oprintf(fp, "  Maximum Voltage: Unknown\n");
	}

	if (md.smbmd_confvolt != 0) {
		oprintf(fp, "  Configured Voltage: %.2fV\n",
		    md.smbmd_confvolt / 1000.0);
	} else {
		oprintf(fp, "  Configured Voltage: Unknown\n");
	}

	if (md.smbmd_memtech != 0) {
		desc_printf(smbios_memdevice_memtech_desc(md.smbmd_memtech),
		    fp, "  Memory Technology: %u", md.smbmd_memtech);
	}

	if (md.smbmd_opcap_flags != 0) {
		flag_printf(fp, "  Operating Mode Capabilities",
		    md.smbmd_opcap_flags, sizeof (md.smbmd_opcap_flags) * NBBY,
		    smbios_memdevice_op_capab_name,
		    smbios_memdevice_op_capab_desc);
	}

	if (md.smbmd_firmware_rev[0] != '\0') {
		oprintf(fp, "  Firmware Revision: %s\n", md.smbmd_firmware_rev);
	}

	if (md.smbmd_modmfg_id != 0) {
		jedec_print(fp, "Module Manufacturer ID", md.smbmd_modmfg_id);
	}

	if (md.smbmd_modprod_id  != 0) {
		jedec_print(fp, "Module Product ID", md.smbmd_modprod_id);
	}

	if (md.smbmd_cntrlmfg_id != 0) {
		jedec_print(fp, "Memory Subsystem Controller Manufacturer ID",
		    md.smbmd_cntrlmfg_id);
	}

	if (md.smbmd_cntrlprod_id != 0) {
		jedec_print(fp, "Memory Subsystem Controller Product ID",
		    md.smbmd_cntrlprod_id);
	}

	if (md.smbmd_nvsize == UINT64_MAX) {
		oprintf(fp, "  Non-volatile Size: Unknown\n");
	} else if (md.smbmd_nvsize != 0) {
		oprintf(fp, "  Non-volatile Size: %llu bytes\n",
		    (u_longlong_t)md.smbmd_nvsize);
	}

	if (md.smbmd_volatile_size == UINT64_MAX) {
		oprintf(fp, "  Volatile Size: Unknown\n");
	} else if (md.smbmd_volatile_size != 0) {
		oprintf(fp, "  Volatile Size: %llu bytes\n",
		    (u_longlong_t)md.smbmd_volatile_size);
	}

	if (md.smbmd_cache_size == UINT64_MAX) {
		oprintf(fp, "  Cache Size: Unknown\n");
	} else if (md.smbmd_cache_size != 0) {
		oprintf(fp, "  Cache Size: %llu bytes\n",
		    (u_longlong_t)md.smbmd_cache_size);
	}

	if (md.smbmd_logical_size == UINT64_MAX) {
		oprintf(fp, "  Logical Size: Unknown\n");
	} else if (md.smbmd_logical_size != 0) {
		oprintf(fp, "  Logical Size: %llu bytes\n",
		    (u_longlong_t)md.smbmd_logical_size);
	}
}

static void
print_memarrmap(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_memarrmap_t ma;

	(void) smbios_info_memarrmap(shp, id, &ma);

	id_printf(fp, "  Physical Memory Array: ", ma.smbmam_array);
	oprintf(fp, "  Devices per Row: %u\n", ma.smbmam_width);

	oprintf(fp, "  Physical Address: 0x%llx\n  Size: %llu bytes\n",
	    (u_longlong_t)ma.smbmam_addr, (u_longlong_t)ma.smbmam_size);
}

static void
print_memdevmap(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_memdevmap_t md;

	(void) smbios_info_memdevmap(shp, id, &md);

	id_printf(fp, "  Memory Device: ", md.smbmdm_device);
	id_printf(fp, "  Memory Array Mapped Address: ", md.smbmdm_arrmap);

	oprintf(fp, "  Physical Address: 0x%llx\n  Size: %llu bytes\n",
	    (u_longlong_t)md.smbmdm_addr, (u_longlong_t)md.smbmdm_size);

	oprintf(fp, "  Partition Row Position: %u\n", md.smbmdm_rpos);
	oprintf(fp, "  Interleave Position: %u\n", md.smbmdm_ipos);
	oprintf(fp, "  Interleave Data Depth: %u\n", md.smbmdm_idepth);
}

static void
print_hwsec(smbios_hdl_t *shp, FILE *fp)
{
	smbios_hwsec_t h;

	(void) smbios_info_hwsec(shp, &h);

	desc_printf(smbios_hwsec_desc(h.smbh_pwr_ps),
	    fp, "  Power-On Password Status: %u", h.smbh_pwr_ps);
	desc_printf(smbios_hwsec_desc(h.smbh_kbd_ps),
	    fp, "  Keyboard Password Status: %u", h.smbh_kbd_ps);
	desc_printf(smbios_hwsec_desc(h.smbh_adm_ps),
	    fp, "  Administrator Password Status: %u", h.smbh_adm_ps);
	desc_printf(smbios_hwsec_desc(h.smbh_pan_ps),
	    fp, "  Front Panel Reset Status: %u", h.smbh_pan_ps);
}

static void
print_vprobe(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_vprobe_t vp;

	if (smbios_info_vprobe(shp, id, &vp) != 0) {
		smbios_warn(shp, "failed to read voltage probe information");
		return;
	}

	oprintf(fp, "  Description: %s\n", vp.smbvp_description != NULL ?
	    vp.smbvp_description : "unknown");
	desc_printf(smbios_vprobe_loc_desc(vp.smbvp_location),
	    fp, "  Location: %u", vp.smbvp_location);
	desc_printf(smbios_vprobe_status_desc(vp.smbvp_status),
	    fp, "  Status: %u", vp.smbvp_status);

	if (vp.smbvp_maxval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Maximum Possible Voltage: %u mV\n",
		    vp.smbvp_maxval);
	} else {
		oprintf(fp, "  Maximum Possible Voltage: unknown\n");
	}

	if (vp.smbvp_minval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Minimum Possible Voltage: %u mV\n",
		    vp.smbvp_minval);
	} else {
		oprintf(fp, "  Minimum Possible Voltage: unknown\n");
	}

	if (vp.smbvp_resolution != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Resolution: %u.%u mV\n",
		    vp.smbvp_resolution / 10,
		    vp.smbvp_resolution % 10);
	} else {
		oprintf(fp, "  Probe Resolution: unknown\n");
	}

	if (vp.smbvp_tolerance != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Tolerance: +/-%u mV\n",
		    vp.smbvp_tolerance);
	} else {
		oprintf(fp, "  Probe Tolerance: unknown\n");
	}

	if (vp.smbvp_accuracy != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Accuracy: +/-%u.%02u%%\n",
		    vp.smbvp_accuracy / 100,
		    vp.smbvp_accuracy % 100);
	} else {
		oprintf(fp, "  Probe Accuracy: unknown\n");
	}

	oprintf(fp, "  OEM- or BIOS- defined value: 0x%x\n", vp.smbvp_oem);

	if (vp.smbvp_nominal != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Nominal Value: %u mV\n", vp.smbvp_nominal);
	} else {
		oprintf(fp, "  Probe Nominal Value: unknown\n");
	}
}

static void
print_cooldev(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_cooldev_t cd;

	if (smbios_info_cooldev(shp, id, &cd) != 0) {
		smbios_warn(shp, "failed to read cooling device "
		    "information");
		return;
	}

	id_printf(fp, "  Temperature Probe Handle: ", cd.smbcd_tprobe);
	desc_printf(smbios_cooldev_type_desc(cd.smbcd_type),
	    fp, "  Device Type: %u", cd.smbcd_type);
	desc_printf(smbios_cooldev_status_desc(cd.smbcd_status),
	    fp, "  Status: %u", cd.smbcd_status);
	oprintf(fp, "  Cooling Unit Group: %u\n", cd.smbcd_group);
	oprintf(fp, "  OEM- or BIOS- defined data: 0x%x\n", cd.smbcd_oem);
	if (cd.smbcd_nominal != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Nominal Speed: %u RPM\n", cd.smbcd_nominal);
	} else {
		oprintf(fp, "  Nominal Speed: unknown\n");
	}

	if (cd.smbcd_descr != NULL && cd.smbcd_descr[0] != '\0') {
		oprintf(fp, "  Description: %s\n", cd.smbcd_descr);
	}
}

static void
print_tprobe(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_tprobe_t tp;

	if (smbios_info_tprobe(shp, id, &tp) != 0) {
		smbios_warn(shp, "failed to read temperature probe "
		    "information");
		return;
	}

	oprintf(fp, "  Description: %s\n", tp.smbtp_description != NULL ?
	    tp.smbtp_description : "unknown");
	desc_printf(smbios_tprobe_loc_desc(tp.smbtp_location),
	    fp, "  Location: %u", tp.smbtp_location);
	desc_printf(smbios_tprobe_status_desc(tp.smbtp_status),
	    fp, "  Status: %u", tp.smbtp_status);

	if (tp.smbtp_maxval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Maximum Possible Temperature: %u.%u C\n",
		    tp.smbtp_maxval / 10, tp.smbtp_maxval % 10);
	} else {
		oprintf(fp, "  Maximum Possible Temperature: unknown\n");
	}

	if (tp.smbtp_minval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Minimum Possible Temperature: %u.%u C\n",
		    tp.smbtp_minval / 10, tp.smbtp_minval % 10);
	} else {
		oprintf(fp, "  Minimum Possible Temperature: unknown\n");
	}

	if (tp.smbtp_resolution != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Resolution: %u.%03u C\n",
		    tp.smbtp_resolution / 1000,
		    tp.smbtp_resolution % 1000);
	} else {
		oprintf(fp, "  Probe Resolution: unknown\n");
	}

	if (tp.smbtp_tolerance != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Tolerance: +/-%u.%u C\n",
		    tp.smbtp_tolerance / 10, tp.smbtp_tolerance % 10);
	} else {
		oprintf(fp, "  Probe Tolerance: unknown\n");
	}

	if (tp.smbtp_accuracy != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Accuracy: +/-%u.%02u%%\n",
		    tp.smbtp_accuracy / 100,
		    tp.smbtp_accuracy % 100);
	} else {
		oprintf(fp, "  Probe Accuracy: unknown\n");
	}

	oprintf(fp, "  OEM- or BIOS- defined value: 0x%x\n", tp.smbtp_oem);

	if (tp.smbtp_nominal != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Nominal Value: %u.%u C\n",
		    tp.smbtp_nominal / 10, tp.smbtp_nominal % 10);
	} else {
		oprintf(fp, "  Probe Nominal Value: unknown\n");
	}
}

static void
print_iprobe(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_iprobe_t ip;

	if (smbios_info_iprobe(shp, id, &ip) != 0) {
		smbios_warn(shp, "failed to read current probe information");
		return;
	}

	oprintf(fp, "  Description: %s\n", ip.smbip_description != NULL ?
	    ip.smbip_description : "unknown");
	desc_printf(smbios_iprobe_loc_desc(ip.smbip_location),
	    fp, "  Location: %u", ip.smbip_location);
	desc_printf(smbios_iprobe_status_desc(ip.smbip_status),
	    fp, "  Status: %u", ip.smbip_status);

	if (ip.smbip_maxval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Maximum Possible Current: %u mA\n",
		    ip.smbip_maxval);
	} else {
		oprintf(fp, "  Maximum Possible Current: unknown\n");
	}

	if (ip.smbip_minval != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Minimum Possible Current: %u mA\n",
		    ip.smbip_minval);
	} else {
		oprintf(fp, "  Minimum Possible Current: unknown\n");
	}

	if (ip.smbip_resolution != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Resolution: %u.%u mA\n",
		    ip.smbip_resolution / 10,
		    ip.smbip_resolution % 10);
	} else {
		oprintf(fp, "  Probe Resolution: unknown\n");
	}

	if (ip.smbip_tolerance != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Tolerance: +/-%u mA\n",
		    ip.smbip_tolerance);
	} else {
		oprintf(fp, "  Probe Tolerance: unknown\n");
	}

	if (ip.smbip_accuracy != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Accuracy: +/-%u.%02u%%\n",
		    ip.smbip_accuracy / 100,
		    ip.smbip_accuracy % 100);
	} else {
		oprintf(fp, "  Probe Accuracy: unknown\n");
	}

	oprintf(fp, "  OEM- or BIOS- defined value: 0x%x\n", ip.smbip_oem);

	if (ip.smbip_nominal != SMB_PROBE_UNKNOWN_VALUE) {
		oprintf(fp, "  Probe Nominal Value: %u mA\n", ip.smbip_nominal);
	} else {
		oprintf(fp, "  Probe Nominal Value: unknown\n");
	}
}


static void
print_boot(smbios_hdl_t *shp, FILE *fp)
{
	smbios_boot_t b;

	(void) smbios_info_boot(shp, &b);

	desc_printf(smbios_boot_desc(b.smbt_status),
	    fp, "  Boot Status Code: 0x%x", b.smbt_status);

	if (b.smbt_size != 0) {
		oprintf(fp, "  Boot Data (%lu bytes):\n", (ulong_t)b.smbt_size);
		print_bytes(b.smbt_data, b.smbt_size, fp);
	}
}

static void
print_ipmi(smbios_hdl_t *shp, FILE *fp)
{
	smbios_ipmi_t i;

	(void) smbios_info_ipmi(shp, &i);

	desc_printf(smbios_ipmi_type_desc(i.smbip_type),
	    fp, "  Type: %u", i.smbip_type);

	oprintf(fp, "  BMC IPMI Version: %u.%u\n",
	    i.smbip_vers.smbv_major, i.smbip_vers.smbv_minor);

	oprintf(fp, "  i2c Bus Slave Address: 0x%x\n", i.smbip_i2c);
	oprintf(fp, "  NV Storage Device Bus ID: 0x%x\n", i.smbip_bus);
	oprintf(fp, "  BMC Base Address: 0x%llx\n", (u_longlong_t)i.smbip_addr);
	oprintf(fp, "  Interrupt Number: %u\n", i.smbip_intr);
	oprintf(fp, "  Register Spacing: %u\n", i.smbip_regspacing);

	flag_printf(fp, "Flags", i.smbip_flags, sizeof (i.smbip_flags) * NBBY,
	    smbios_ipmi_flag_name, smbios_ipmi_flag_desc);
}

static void
print_powersup(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_powersup_t p;

	if (smbios_info_powersup(shp, id, &p) != 0) {
		smbios_warn(shp, "failed to read power supply information");
		return;
	}

	oprintf(fp, "  Power Supply Group: %u\n", p.smbps_group);
	if (p.smbps_maxout != 0x8000) {
		oprintf(fp, "  Maximum Output: %llu mW\n", p.smbps_maxout);
	} else {
		oprintf(fp, "  Maximum Output: unknown\n");
	}

	flag_printf(fp, "Characteristics", p.smbps_flags,
	    sizeof (p.smbps_flags) * NBBY, smbios_powersup_flag_name,
	    smbios_powersup_flag_desc);

	desc_printf(smbios_powersup_input_desc(p.smbps_ivrs),
	    fp, "  Input Voltage Range Switching: %u", p.smbps_ivrs);
	desc_printf(smbios_powersup_status_desc(p.smbps_status),
	    fp, "  Status: %u", p.smbps_status);
	desc_printf(smbios_powersup_type_desc(p.smbps_pstype),
	    fp, "  Type: %u", p.smbps_pstype);

	if (p.smbps_vprobe != 0xffff) {
		oprintf(fp, "  Voltage Probe Handle: %lu\n", p.smbps_vprobe);
	}

	if (p.smbps_cooldev != 0xffff) {
		oprintf(fp, "  Cooling Device Handle: %lu\n", p.smbps_cooldev);
	}

	if (p.smbps_iprobe != 0xffff) {
		oprintf(fp, "  Current Probe Handle: %lu\n", p.smbps_iprobe);
	}
}

static void
print_processor_info_riscv(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_processor_info_riscv_t rv;

	if (smbios_info_processor_riscv(shp, id, &rv) != 0) {
		smbios_warn(shp, "failed to read RISC-V specific processor "
		    "information");
		return;
	}

	if (rv.smbpirv_boothart != 0) {
		oprintf(fp, "    Boot Hart\n");
	}
	u128_print(fp, "    Hart ID", rv.smbpirv_hartid);
	u128_print(fp, "    Vendor ID", rv.smbpirv_vendid);
	u128_print(fp, "    Architecture ID", rv.smbpirv_archid);
	u128_print(fp, "    Implementation ID", rv.smbpirv_machid);
	flag64_printf(fp, "  ISA", rv.smbpirv_isa,
	    sizeof (rv.smbpirv_isa) * NBBY, smbios_riscv_isa_name,
	    smbios_riscv_isa_desc);
	flag_printf(fp, "  Privilege Levels", rv.smbpirv_privlvl,
	    sizeof (rv.smbpirv_privlvl) * NBBY, smbios_riscv_priv_name,
	    smbios_riscv_priv_desc);
	u128_print(fp, "    Machine Exception Trap Delegation",
	    rv.smbpirv_metdi);
	u128_print(fp, "    Machine Interrupt Trap Delegation",
	    rv.smbpirv_mitdi);
	desc_printf(smbios_riscv_width_desc(rv.smbpirv_xlen),
	    fp, "    Register Width: 0x%x", rv.smbpirv_xlen);
	desc_printf(smbios_riscv_width_desc(rv.smbpirv_mxlen),
	    fp, "    M-Mode Register Width: 0x%x", rv.smbpirv_mxlen);
	desc_printf(smbios_riscv_width_desc(rv.smbpirv_sxlen),
	    fp, "    S-Mode Register Width: 0x%x", rv.smbpirv_sxlen);
	desc_printf(smbios_riscv_width_desc(rv.smbpirv_uxlen),
	    fp, "    U-Mode Register Width: 0x%x", rv.smbpirv_uxlen);
}

static void
print_processor_info(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_processor_info_t p;

	if (smbios_info_processor_info(shp, id, &p) != 0) {
		smbios_warn(shp, "failed to read processor additional "
		    "information");
		return;
	}

	id_printf(fp, "  Processor Handle: ", p.smbpi_processor);
	desc_printf(smbios_processor_info_type_desc(p.smbpi_ptype),
	    fp, "  Processor Type: %u", p.smbpi_ptype);

	switch (p.smbpi_ptype) {
	case SMB_PROCINFO_T_RV32:
	case SMB_PROCINFO_T_RV64:
	case SMB_PROCINFO_T_RV128:
		oprintf(fp, "  RISC-V Additional Processor Information:\n");
		print_processor_info_riscv(shp, id, fp);
		break;
	default:
		break;
	}
}

static void
print_pointdev(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_pointdev_t pd;

	if (smbios_info_pointdev(shp, id, &pd) != 0) {
		smbios_warn(shp, "failed to read pointer device information");
		return;
	}

	desc_printf(smbios_pointdev_type_desc(pd.smbpd_type),
	    fp, "  Type: %u", pd.smbpd_type);
	desc_printf(smbios_pointdev_iface_desc(pd.smbpd_iface),
	    fp, "  Interface: %u", pd.smbpd_iface);
	oprintf(fp, "  Buttons: %u\n", pd.smbpd_nbuttons);
}

static void
print_extprocessor(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	int i;
	smbios_processor_ext_t ep;

	if (check_oem(shp) != 0)
		return;

	(void) smbios_info_extprocessor(shp, id, &ep);

	oprintf(fp, "  Processor: %u\n", ep.smbpe_processor);
	oprintf(fp, "  FRU: %u\n", ep.smbpe_fru);
	oprintf(fp, "  Initial APIC ID count: %u\n\n", ep.smbpe_n);

	for (i = 0; i < ep.smbpe_n; i++) {
		oprintf(fp, "  Logical Strand %u: Initial APIC ID: %u\n", i,
		    ep.smbpe_apicid[i]);
	}
}

static void
print_extport(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_port_ext_t epo;

	if (check_oem(shp) != 0)
		return;

	(void) smbios_info_extport(shp, id, &epo);

	oprintf(fp, "  Chassis Handle: %u\n", epo.smbporte_chassis);
	oprintf(fp, "  Port Connector Handle: %u\n", epo.smbporte_port);
	oprintf(fp, "  Device Type: %u\n", epo.smbporte_dtype);
	oprintf(fp, "  Device Handle: %u\n", epo.smbporte_devhdl);
	oprintf(fp, "  PHY: %u\n", epo.smbporte_phy);
}

static void
print_pciexrc(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_pciexrc_t pcie;

	if (check_oem(shp) != 0)
		return;

	(void) smbios_info_pciexrc(shp, id, &pcie);

	oprintf(fp, "  Component ID: %u\n", pcie.smbpcie_bb);
	oprintf(fp, "  BDF: 0x%x\n", pcie.smbpcie_bdf);
}

static void
print_extmemarray(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	smbios_memarray_ext_t em;

	if (check_oem(shp) != 0)
		return;

	(void) smbios_info_extmemarray(shp, id, &em);

	oprintf(fp, "  Physical Memory Array Handle: %u\n", em.smbmae_ma);
	oprintf(fp, "  Component Parent Handle: %u\n", em.smbmae_comp);
	oprintf(fp, "  BDF: 0x%x\n", em.smbmae_bdf);
}

static void
print_extmemdevice(smbios_hdl_t *shp, id_t id, FILE *fp)
{
	int i;
	smbios_memdevice_ext_t emd;

	if (check_oem(shp) != 0)
		return;

	(void) smbios_info_extmemdevice(shp, id, &emd);

	oprintf(fp, "  Memory Device Handle: %u\n", emd.smbmdeve_md);
	oprintf(fp, "  DRAM Channel: %u\n", emd.smbmdeve_drch);
	oprintf(fp, "  Number of Chip Selects: %u\n", emd.smbmdeve_ncs);

	for (i = 0; i < emd.smbmdeve_ncs; i++) {
		oprintf(fp, "  Chip Select: %u\n", emd.smbmdeve_cs[i]);
	}
}

static int
print_struct(smbios_hdl_t *shp, const smbios_struct_t *sp, void *fp)
{
	smbios_info_t info;
	int hex = opt_x;
	const char *s;

	if (opt_t != -1 && opt_t != sp->smbstr_type)
		return (0); /* skip struct if type doesn't match -t */

	if (!opt_O && (sp->smbstr_type == SMB_TYPE_MEMCTL ||
	    sp->smbstr_type == SMB_TYPE_MEMMOD))
		return (0); /* skip struct if type is obsolete */

	if (g_hdr++ == 0 || !opt_s)
		oprintf(fp, "%-5s %-4s %s\n", "ID", "SIZE", "TYPE");

	oprintf(fp, "%-5u %-4lu",
	    (uint_t)sp->smbstr_id, (ulong_t)sp->smbstr_size);

	if ((s = smbios_type_name(sp->smbstr_type)) != NULL)
		oprintf(fp, " %s (type %u)", s, sp->smbstr_type);
	else if (sp->smbstr_type > SMB_TYPE_OEM_LO &&
	    sp->smbstr_type < SMB_TYPE_OEM_HI)
		oprintf(fp, " %s+%u (type %u)", "SMB_TYPE_OEM_LO",
		    sp->smbstr_type - SMB_TYPE_OEM_LO, sp->smbstr_type);
	else
		oprintf(fp, " %u", sp->smbstr_type);

	if ((s = smbios_type_desc(sp->smbstr_type)) != NULL)
		oprintf(fp, " (%s)\n", s);
	else
		oprintf(fp, "\n");

	if (opt_s)
		return (0); /* only print header line if -s specified */

	if (smbios_info_common(shp, sp->smbstr_id, &info) == 0) {
		oprintf(fp, "\n");
		print_common(&info, fp);
	}

	switch (sp->smbstr_type) {
	case SMB_TYPE_BIOS:
		oprintf(fp, "\n");
		print_bios(shp, fp);
		break;
	case SMB_TYPE_SYSTEM:
		oprintf(fp, "\n");
		print_system(shp, fp);
		break;
	case SMB_TYPE_BASEBOARD:
		oprintf(fp, "\n");
		print_bboard(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_CHASSIS:
		oprintf(fp, "\n");
		print_chassis(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_PROCESSOR:
		oprintf(fp, "\n");
		print_processor(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_CACHE:
		oprintf(fp, "\n");
		print_cache(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_PORT:
		oprintf(fp, "\n");
		print_port(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_SLOT:
		oprintf(fp, "\n");
		print_slot(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_OBDEVS:
		oprintf(fp, "\n");
		print_obdevs(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_OEMSTR:
	case SMB_TYPE_SYSCONFSTR:
		oprintf(fp, "\n");
		print_strtab(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_LANG:
		oprintf(fp, "\n");
		print_lang(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_EVENTLOG:
		oprintf(fp, "\n");
		print_evlog(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_MEMARRAY:
		oprintf(fp, "\n");
		print_memarray(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_MEMDEVICE:
		oprintf(fp, "\n");
		print_memdevice(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_MEMARRAYMAP:
		oprintf(fp, "\n");
		print_memarrmap(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_MEMDEVICEMAP:
		oprintf(fp, "\n");
		print_memdevmap(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_POINTDEV:
		oprintf(fp, "\n");
		print_pointdev(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_SECURITY:
		oprintf(fp, "\n");
		print_hwsec(shp, fp);
		break;
	case SMB_TYPE_VPROBE:
		oprintf(fp, "\n");
		print_vprobe(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_COOLDEV:
		oprintf(fp, "\n");
		print_cooldev(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_TPROBE:
		oprintf(fp, "\n");
		print_tprobe(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_IPROBE:
		oprintf(fp, "\n");
		print_iprobe(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_BOOT:
		oprintf(fp, "\n");
		print_boot(shp, fp);
		break;
	case SMB_TYPE_IPMIDEV:
		oprintf(fp, "\n");
		print_ipmi(shp, fp);
		break;
	case SMB_TYPE_POWERSUP:
		oprintf(fp, "\n");
		print_powersup(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_OBDEVEXT:
		oprintf(fp, "\n");
		print_obdevs_ext(shp, sp->smbstr_id, fp);
		break;
	case SMB_TYPE_PROCESSOR_INFO:
		oprintf(fp, "\n");
		print_processor_info(shp, sp->smbstr_id, fp);
		break;
	case SUN_OEM_EXT_PROCESSOR:
		oprintf(fp, "\n");
		print_extprocessor(shp, sp->smbstr_id, fp);
		break;
	case SUN_OEM_EXT_PORT:
		oprintf(fp, "\n");
		print_extport(shp, sp->smbstr_id, fp);
		break;
	case SUN_OEM_PCIEXRC:
		oprintf(fp, "\n");
		print_pciexrc(shp, sp->smbstr_id, fp);
		break;
	case SUN_OEM_EXT_MEMARRAY:
		oprintf(fp, "\n");
		print_extmemarray(shp, sp->smbstr_id, fp);
		break;
	case SUN_OEM_EXT_MEMDEVICE:
		oprintf(fp, "\n");
		print_extmemdevice(shp, sp->smbstr_id, fp);
		break;
	default:
		hex++;
	}

	if (hex)
		print_bytes(sp->smbstr_data, sp->smbstr_size, fp);
	else
		oprintf(fp, "\n");

	return (0);
}

static uint16_t
getu16(const char *name, const char *s)
{
	u_longlong_t val;
	char *p;

	errno = 0;
	val = strtoull(s, &p, 0);

	if (errno != 0 || p == s || *p != '\0' || val > UINT16_MAX) {
		(void) fprintf(stderr, "%s: invalid %s argument -- %s\n",
		    g_pname, name, s);
		exit(SMBIOS_USAGE);
	}

	return ((uint16_t)val);
}

static uint16_t
getstype(const char *name, const char *s)
{
	const char *ts;
	uint16_t t;

	for (t = 0; t < SMB_TYPE_OEM_LO; t++) {
		if ((ts = smbios_type_name(t)) != NULL && strcmp(s, ts) == 0)
			return (t);
	}

	(void) fprintf(stderr, "%s: invalid %s argument -- %s\n",
	    g_pname, name, s);

	exit(SMBIOS_USAGE);
	/*NOTREACHED*/
}

static int
usage(FILE *fp)
{
	(void) fprintf(fp, "Usage: %s "
	    "[-BeOsx] [-i id] [-t type] [-w file] [file]\n\n", g_pname);

	(void) fprintf(fp,
	    "\t-B disable header validation for broken BIOSes\n"
	    "\t-e display SMBIOS entry point information\n"
	    "\t-i display only the specified structure\n"
	    "\t-O display obsolete structure types\n"
	    "\t-s display only a summary of structure identifiers and types\n"
	    "\t-t display only the specified structure type\n"
	    "\t-w write the raw data to the specified file\n"
	    "\t-x display raw data for structures\n");

	return (SMBIOS_USAGE);
}

int
main(int argc, char *argv[])
{
	const char *ifile = NULL;
	const char *ofile = NULL;
	int oflags = 0;

	smbios_hdl_t *shp;
	smbios_struct_t s;
	int err, fd, c;
	char *p;

	if ((p = strrchr(argv[0], '/')) == NULL)
		g_pname = argv[0];
	else
		g_pname = p + 1;

	while (optind < argc) {
		while ((c = getopt(argc, argv, "Bei:Ost:w:xZ")) != EOF) {
			switch (c) {
			case 'B':
				oflags |= SMB_O_NOCKSUM | SMB_O_NOVERS;
				break;
			case 'e':
				opt_e++;
				break;
			case 'i':
				opt_i = getu16("struct ID", optarg);
				break;
			case 'O':
				opt_O++;
				break;
			case 's':
				opt_s++;
				break;
			case 't':
				if (isdigit(optarg[0]))
					opt_t = getu16("struct type", optarg);
				else
					opt_t = getstype("struct type", optarg);
				break;
			case 'w':
				ofile = optarg;
				break;
			case 'x':
				opt_x++;
				break;
			case 'Z':
				oflags |= SMB_O_ZIDS; /* undocumented */
				break;
			default:
				return (usage(stderr));
			}
		}

		if (optind < argc) {
			if (ifile != NULL) {
				(void) fprintf(stderr, "%s: illegal "
				    "argument -- %s\n", g_pname, argv[optind]);
				return (SMBIOS_USAGE);
			}
			ifile = argv[optind++];
		}
	}

	if ((shp = smbios_open(ifile, SMB_VERSION, oflags, &err)) == NULL) {
		(void) fprintf(stderr, "%s: failed to load SMBIOS: %s\n",
		    g_pname, smbios_errmsg(err));
		return (SMBIOS_ERROR);
	}

	if (opt_i == -1 && opt_t == -1 && opt_e == 0 &&
	    smbios_truncated(shp))
		(void) fprintf(stderr, "%s: SMBIOS table is truncated\n",
		    g_pname);

	if (ofile != NULL) {
		if ((fd = open(ofile, O_WRONLY|O_CREAT|O_TRUNC, 0666)) == -1) {
			(void) fprintf(stderr, "%s: failed to open %s: %s\n",
			    g_pname, ofile, strerror(errno));
			err = SMBIOS_ERROR;
		} else if (smbios_write(shp, fd) != 0) {
			(void) fprintf(stderr, "%s: failed to write %s: %s\n",
			    g_pname, ofile, smbios_errmsg(smbios_errno(shp)));
			err = SMBIOS_ERROR;
		}
		smbios_close(shp);
		return (err);
	}

	if (opt_e) {
		print_smbios(shp, stdout);
		smbios_close(shp);
		return (SMBIOS_SUCCESS);
	}

	if (opt_O && (opt_i != -1 || opt_t != -1))
		opt_O++; /* -i or -t imply displaying obsolete records */

	if (opt_i != -1)
		err = smbios_lookup_id(shp, opt_i, &s);
	else
		err = smbios_iter(shp, print_struct, stdout);

	if (err != 0) {
		(void) fprintf(stderr, "%s: failed to access SMBIOS: %s\n",
		    g_pname, smbios_errmsg(smbios_errno(shp)));
		smbios_close(shp);
		return (SMBIOS_ERROR);
	}

	if (opt_i != -1)
		(void) print_struct(shp, &s, stdout);

	smbios_close(shp);
	return (SMBIOS_SUCCESS);
}
