/*
 * Copyright (C) 2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

// Code in this file gets included in the unit tests.
#include "generator-internals.c"

// Logging

#define KMSG_DEV_PATH		"/dev/kmsg"
static int _kmsg_fd;

static void _log_init(void)
{
	// failing is harmless
	_kmsg_fd = open(KMSG_DEV_PATH, O_WRONLY | O_NOCTTY);
}

static void _log_exit(void)
{
	if (_kmsg_fd != -1)
		(void) close(_kmsg_fd);
}

__attribute__ ((format(printf, 1, 2)))
static void _error(const char *format, ...)
{
	int n;
	va_list ap;
	char message[PATH_MAX + 30];	/* +3 for '<n>' where n is the log level and +27 for lvm2-activation-generator: " prefix */

	snprintf(message, 31, "<%d>lvm2-activation-generator: ", LOG_ERR);

	va_start(ap, format);
	n = vsnprintf(message + 30, PATH_MAX, format, ap);
	va_end(ap);

	if (_kmsg_fd < 0 || (n < 0 || ((unsigned) n + 1 > PATH_MAX)))
		return;

	/* The n+31: +30 for "<n>lvm2-activation-generator: " prefix and +1 for '\0' suffix */
	if (write(_kmsg_fd, message, n + 31) < 0)
		_error("Failed to write activation message %s: %m.\n", message);
}

//----------------------------------------------------------------

#define UNIT_TARGET_LOCAL_FS  "local-fs-pre.target"
#define UNIT_TARGET_REMOTE_FS "remote-fs-pre.target"

struct generator {
	const char *dir;
	struct config cfg;

	int kmsg_fd;
	char unit_path[PATH_MAX];
	char target_path[PATH_MAX];
};

enum {
	UNIT_EARLY,
	UNIT_MAIN,
	UNIT_NET
};

static const char *_unit_names[] = {
	[UNIT_EARLY] = "lvm2-activation-early.service",
	[UNIT_MAIN] = "lvm2-activation.service",
	[UNIT_NET] = "lvm2-activation-net.service"
};

//----------------------------------------------------------------

static int register_unit_with_target(struct generator *gen, const char *unit,
				     const char *target)
{
	int r = 1;

	if (dm_snprintf(gen->target_path, PATH_MAX, "%s/%s.wants", gen->dir, target) < 0) {
		r = 0;
		goto out;
	}

	(void) dm_prepare_selinux_context(gen->target_path, S_IFDIR);
	if (mkdir(gen->target_path, 0755) < 0 && errno != EEXIST) {
		_error("Failed to create target directory %s: %m.\n", gen->target_path);
		r = 0;
		goto out;
	}

	if (dm_snprintf
	    (gen->target_path, PATH_MAX, "%s/%s.wants/%s", gen->dir, target, unit) < 0) {
		r = 0;
		goto out;
	}
	(void) dm_prepare_selinux_context(gen->target_path, S_IFLNK);
	if (symlink(gen->unit_path, gen->target_path) < 0) {
		_error("Failed to create symlink for unit %s: %m.\n", unit);
		r = 0;
	}
 out:
	dm_prepare_selinux_context(NULL, 0);
	return r;
}

static int generate_unit(struct generator *gen, int unit)
{
	FILE *f;
	const char *unit_name = _unit_names[unit];
	const char *target_name =
	    unit == UNIT_NET ? UNIT_TARGET_REMOTE_FS : UNIT_TARGET_LOCAL_FS;

	if (dm_snprintf(gen->unit_path, PATH_MAX, "%s/%s", gen->dir, unit_name)
	    < 0)
		return 0;

	if (!(f = fopen(gen->unit_path, "wxe"))) {
		_error("Failed to create unit file %s: %m.\n", unit_name);
		return 0;
	}

	fputs("# Automatically generated by lvm2-activation-generator.\n"
	      "#\n"
	      "# This unit is responsible for direct activation of LVM logical volumes\n"
	      "# if event-based activation not used (global/event_activation=0 in\n"
	      "# lvm.conf). Direct LVM activation requires udev to be settled!\n\n"
	      "[Unit]\n"
	      "Description=LVM direct activation of logical volumes\n"
	      "Documentation=man:lvm2-activation-generator(8)\n"
	      "SourcePath=/etc/lvm/lvm.conf\n" "DefaultDependencies=no\n", f);

	fputs("Conflicts=shutdown.target\n", f);

	if (unit == UNIT_NET) {
		fprintf(f, "After=%s iscsi.service fcoe.service rbdmap.service\n"
			"Before=remote-fs-pre.target shutdown.target\n\n"
			"[Service]\n"
			"ExecStartPre=/usr/bin/udevadm settle\n", _unit_names[UNIT_MAIN]);
	} else {
		if (unit == UNIT_EARLY)
			fputs("After=systemd-udev-settle.service\n"
			      "Before=cryptsetup.target\n", f);
		else
			fprintf(f, "After=%s cryptsetup.target\n", _unit_names[UNIT_EARLY]);

		fputs("Before=local-fs-pre.target shutdown.target\n"
		      "Wants=systemd-udev-settle.service\n\n" "[Service]\n", f);
	}

	fputs("ExecStart=" LVM_PATH " vgchange -aay", f);
	if (gen->cfg.sysinit_needed)
		fputs(" --sysinit", f);
	fputs("\nType=oneshot\n", f);

	if (fclose(f) < 0) {
		_error("Failed to write unit file %s: %m.\n", unit_name);
		return 0;
	}

	if (!register_unit_with_target(gen, unit_name, target_name)) {
		_error("Failed to register unit %s with target %s.\n",
		       unit_name, target_name);
		return 0;
	}

	return 1;
}

static bool _parse_command_line(struct generator *gen, int argc, const char **argv)
{
	if (argc != 4) {
		_error("Incorrect number of arguments for activation generator.\n");
		return false;
	}

	gen->dir = argv[1];
	return true;
}

static bool _run(int argc, const char **argv)
{
	bool r;
	mode_t old_mask;
	struct generator gen;

	if (!_parse_command_line(&gen, argc, argv))
		return false;

	if (_get_config(&gen.cfg, LVMCONFIG_PATH)) {
		if (gen.cfg.event_activation)
			// If event_activation=1, pvscan --cache -aay does activation.
			return true;
	}

	/*
	 *  Create the activation units if:
	 *    - _get_config succeeded and event_activation=0
	 *    - _get_config failed, then this is a failsafe fallback
	 */

	/* mark lvm2-activation.*.service as world-accessible */
	old_mask = umask(0022);

	r = generate_unit(&gen, UNIT_EARLY) &&
	    generate_unit(&gen, UNIT_MAIN) && generate_unit(&gen, UNIT_NET);

	umask(old_mask);

	return r;
}

int main(int argc, const char **argv)
{
	bool r;

	_log_init();
	r = _run(argc, argv);
	if (!r)
		_error("Activation generator failed.\n");
	_log_exit();

	return r ? EXIT_SUCCESS : EXIT_FAILURE;
}