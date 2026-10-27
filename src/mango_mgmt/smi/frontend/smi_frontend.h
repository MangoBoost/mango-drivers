// SPDX-License-Identifier: GPL-2.0
/*
 * smi_frontend.h - The UAPI axis: which surface a card presents.
 *
 * A surface is an ABI userspace parses; a wire generation is a property of the
 * card. No row pairs by generation and no wire names a surface -- they meet at
 * the neutral sensor model, which is what makes any pair of the two legal.
 * Which surface a card gets is decided once: the first row whose condition
 * holds.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_FRONTEND_H
#define SMI_FRONTEND_H

#include <linux/hwmon.h>
#include <linux/types.h>

struct device;
struct file_operations;
struct pf_dev_struct;

/**
 * enum smi_uapi_id - The surfaces this build can present.
 * @SMI_UAPI_AUTO: not a surface -- the first row whose condition holds.
 * @SMI_UAPI_LEGACY: the vendor hwmon/ioctl spelling, chip "mango".
 */
enum smi_uapi_id {
	SMI_UAPI_AUTO = 0,
	SMI_UAPI_LEGACY = 1,
};

/**
 * enum smi_face_found - What a face found of the card facts it reads.
 * @SMI_FACE_FOUND_ALL: all it reads, or it reads none.
 * @SMI_FACE_FOUND_MISSING: some the card has none of: it serves MISSING_INFO.
 * @SMI_FACE_FOUND_BROKEN: one it could not read: the card is INIT_ERROR.
 */
enum smi_face_found {
	SMI_FACE_FOUND_ALL = 0,
	SMI_FACE_FOUND_MISSING,
	SMI_FACE_FOUND_BROKEN,
};

/**
 * struct smi_face_ops - A card's user side apart from hwmon: its node and the
 *			 files and state that go with it.
 * @fops: the card node is made with these.
 * @node_mode: the permission that node's /dev entry gets.
 * @present: set up this face's files and per-card state, bound or not, and read
 *	     the facts it serves of a card discovery found whole. Returns an
 *	     enum smi_face_found, or a negative errno with nothing set up.
 * @leave: removal has started; tell the face's users before the reference
 *	   wait.
 * @unpresent: undo @present, once no reference is left.
 */
struct smi_face_ops {
	const struct file_operations *fops;
	umode_t node_mode;
	int (*present)(struct pf_dev_struct *pf_dev);
	void (*leave)(struct pf_dev_struct *pf_dev);
	void (*unpresent)(struct pf_dev_struct *pf_dev);
};

/**
 * struct smi_frontend_ops - One UAPI surface, independent of any wire.
 * @id: what this surface is asked for by.
 * @name: what it is called where a number would not say enough -- the log and
 *	  the sysfs attribute. Not the hwmon chip name, which hwmon publishes.
 * @pairs: whether this surface binds on @pf_dev; rows are asked
 *	   newest first, the first that holds binds, the oldest holding for all.
 * @hwmon_register: present the surface on @dev; call smi_frontend_bound() on
 *		    success. Nothing else may record a binding.
 * @chan_resolve: the record a channel names. The surface numbers its channels,
 *		  so every reader that answers by channel -- hwmon and the
 *		  ioctl alike -- asks here rather than deriving a numbering.
 * @face: the card's user side. Worn from registration, bound or not.
 * @mod_init: set up what this surface keeps for the whole module, or NULL.
 * @mod_exit: undo @mod_init, or NULL.
 */
struct smi_frontend_ops {
	enum smi_uapi_id id;
	const char *name;
	bool (*pairs)(const struct pf_dev_struct *pf_dev);
	int (*hwmon_register)(struct device *dev, struct pf_dev_struct *pf_dev);
	int (*chan_resolve)(struct pf_dev_struct *pf_dev,
			    enum hwmon_sensor_types type, int channel,
			    uint8_t *rid, uint8_t *id);
	const struct smi_face_ops *face;
	int (*mod_init)(void);
	void (*mod_exit)(void);
};

/**
 * smi_frontend_hwmon() - The hwmon device this card presents, or NULL.
 * smi_frontend_priv() - What the bound surface's callbacks read.
 */
struct device *smi_frontend_hwmon(const struct pf_dev_struct *pf_dev);
void *smi_frontend_priv(const struct pf_dev_struct *pf_dev);

/**
 * smi_frontend_of() - The surface an id names.
 *
 * Return: the surface, or NULL for an id this build has none for --
 *	   SMI_UAPI_AUTO among them, which is a question, not a surface.
 */
const struct smi_frontend_ops *smi_frontend_of(enum smi_uapi_id id);

/**
 * smi_frontend_paired() - The surface that binds on this card.
 *
 * Return: the first row whose condition holds, or NULL when none does.
 */
const struct smi_frontend_ops *
smi_frontend_paired(const struct pf_dev_struct *pf_dev);

/**
 * smi_uapi_wanted() - Which surface to ask for on this card.
 * @pf_dev: the card, as far as it settled.
 *
 * Return: the surface id to bind.
 */
enum smi_uapi_id smi_uapi_wanted(const struct pf_dev_struct *pf_dev);

/**
 * smi_frontend_bind() - Present a surface on this card.
 * @pf_dev: the card; the surface hangs off its device.
 * @id: which surface. SMI_UAPI_AUTO is a question, not a surface, and is
 *	refused here -- the caller resolves it before asking.
 *
 * Return: 0, -EINVAL for an id this build has no surface for, else the
 *	   surface's own errno. Nothing is bound on failure, and no other
 *	   surface is presented in its place.
 */
int smi_frontend_bind(struct pf_dev_struct *pf_dev, enum smi_uapi_id id);

/**
 * smi_frontend_bound() - Record the binding a surface has just made.
 * @pf_dev: PCI device data structure.
 * @ops: the surface that bound.
 * @hwmon: the device devm_hwmon_device_register_with_info() returned.
 * @priv: what that surface's callbacks read, or NULL where they read none.
 *
 * The one writer of the whole record, so no reader sees half a binding. The
 * surface pointer is the proof there is one; @priv is only its state.
 *
 * Return: 0, or -EINVAL for a missing argument.
 */
int smi_frontend_bound(struct pf_dev_struct *pf_dev,
		       const struct smi_frontend_ops *ops, struct device *hwmon,
		       void *priv);

/**
 * smi_frontend_chan_resolve() - The record an hwmon channel names.
 * @pf_dev: the device the channel belongs to.
 * @type: hwmon sensor type.
 * @channel: the channel, as the bound surface numbers them.
 * @rid: set to the repository holding the record.
 * @id: set to its ordinal there, one based.
 *
 * Return: 0, -EOPNOTSUPP where no surface is bound, or -ENOENT for a channel
 *	   the bound surface does not have.
 */
int smi_frontend_chan_resolve(struct pf_dev_struct *pf_dev,
			      enum hwmon_sensor_types type, int channel,
			      uint8_t *rid, uint8_t *id);

/* What every surface keeps for the whole module: the class, then each row's. */
int smi_frontend_module_init(void);
void smi_frontend_module_exit(void);

/**
 * smi_frontend_present() - Give a card its user side.
 * @pf_dev: the card, attached and, where it could be, bound.
 * @want: the surface asked for, used where none is bound.
 *
 * The face is the bound surface's, else @want's, else the oldest row's -- a
 * card that bound nothing still gets a node that says why.
 *
 * Return: what the face found (enum smi_face_found) with the node made, or a
 *	   negative error code with nothing left.
 */
int smi_frontend_present(struct pf_dev_struct *pf_dev, enum smi_uapi_id want);

/* Removal has started: the face tells its users, before the reference wait. */
void smi_frontend_leave(struct pf_dev_struct *pf_dev);

/* Undo smi_frontend_present(), once no reference is left. */
void smi_frontend_unpresent(struct pf_dev_struct *pf_dev);

/**
 * smi_frontend_name() - What surface this card presents.
 *
 * Return: the bound surface's name, or NULL when none is bound.
 */
const char *smi_frontend_name(const struct pf_dev_struct *pf_dev);

#endif /* SMI_FRONTEND_H */
