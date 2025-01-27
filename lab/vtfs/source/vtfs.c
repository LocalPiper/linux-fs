#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/string.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

struct vtfs_file {
  struct list_head list;
  char* name;
  ino_t ino;
  umode_t mode;
  struct inode* inode;
  size_t size;
  char* data;
};

struct vtfs_dir {
  struct list_head children;
  struct vtfs_file* self;
};

void vtfs_kill_sb(struct super_block*);
struct dentry* vtfs_mount(struct file_system_type*, int, const char*, void*);
int vtfs_fill_super(struct super_block*, void*, int);
struct inode* vtfs_get_inode(struct super_block*, const struct inode*, umode_t, int);
struct dentry* vtfs_lookup(struct inode*, struct dentry*, unsigned int);
int vtfs_iterate(struct file*, struct dir_context*);
int vtfs_create(struct mnt_idmap*, struct inode*, struct dentry*, umode_t, bool);
int vtfs_unlink(struct inode*, struct dentry*);
int vtfs_mkdir(struct mnt_idmap*, struct inode*, struct dentry*, umode_t);
int vtfs_rmdir(struct inode*, struct dentry*);
ssize_t vtfs_read(struct file*, char __user*, size_t, loff_t*);
ssize_t vtfs_write(struct file*, const char __user*, size_t, loff_t*);
int vtfs_link(struct dentry*, struct inode*, struct dentry*);

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

struct file_operations vtfs_file_ops = {
    .read = vtfs_read,
    .write = vtfs_write,
    .llseek = generic_file_llseek,
};

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
    .mkdir = vtfs_mkdir,
    .rmdir = vtfs_rmdir,
    .link = vtfs_link,
};

ssize_t vtfs_read(struct file* file, char __user* buf, size_t len, loff_t* ppos) {
  struct inode* inode = file_inode(file);
  struct vtfs_file* file_data = inode->i_private;
  size_t available, to_copy;

  if (!file_data || !file_data->data) {
    LOG("No data in file %s\n", file_data ? file_data->name : "NULL");
    return 0;
  }
  available = file_data->size - *ppos;
  if (available <= 0) {
    return 0;
  }

  to_copy = min(len, available);

  if (copy_to_user(buf, file_data->data + *ppos, to_copy)) {
    LOG("Failed to copy data to US\n");
    return -EFAULT;
  }

  *ppos += to_copy;
  LOG("Read %zu bytes from file %s at offset %lld\n", to_copy, file_data->name, *ppos);
  return to_copy;
}

ssize_t vtfs_write(struct file* file, const char __user* buf, size_t len, loff_t* ppos) {
  struct inode* inode = file->f_inode;
  struct vtfs_file* file_data = inode->i_private;
  char* new_data;
  size_t new_size;

  if (!file_data) {
    LOG("Invalid file data\n");
    return -EINVAL;
  }

  new_size = max((size_t)(*ppos + len), file_data->size);

  if (new_size > file_data->size) {
    new_data = krealloc(file_data->data, new_size, GFP_KERNEL);
    if (!new_data) {
      LOG("Realloc failed\n");
      return -ENOMEM;
    }

    if (new_size > file_data->size) {
      memset(new_data + file_data->size, 0, new_size - file_data->size);
    }

    memset(new_data + file_data->size, 0, new_size - file_data->size);
    file_data->data = new_data;
    file_data->size = new_size;
  }

  if (copy_from_user(file_data->data + *ppos, buf, len)) {
    LOG("Failed to copy data from US\n");
    return -EFAULT;
  }

  *ppos += len;
  LOG("Wrote %zu bytes to file %s at offset %lld\n", len, file_data->name, *ppos);

  return len;
}

int vtfs_create(
    struct mnt_idmap* idmap,
    struct inode* parent_inode,
    struct dentry* child_dentry,
    umode_t mode,
    bool excl
) {
  if (S_ISDIR(mode)) {
    LOG("Directory creation is not supported yet\n");
    return -EPERM;
  }

  struct vtfs_dir* parent_dir = parent_inode->i_private;
  struct vtfs_file* new_file;

  struct list_head* pos;
  list_for_each(pos, &parent_dir->children) {
    struct vtfs_file* entry = list_entry(pos, struct vtfs_file, list);
    if (strcmp(entry->name, child_dentry->d_name.name) == 0) {
      return -EEXIST;
    }
  }

  new_file = kmalloc(sizeof(struct vtfs_file), GFP_KERNEL);
  if (!new_file)
    return -ENOMEM;

  new_file->name = kstrdup(child_dentry->d_name.name, GFP_KERNEL);
  new_file->ino = get_next_ino();
  new_file->mode = mode;
  new_file->size = 0;
  new_file->data = NULL;

  new_file->inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, mode, new_file->ino);
  new_file->inode->i_private = new_file;  // happy debugging
  new_file->inode->i_op = &vtfs_inode_ops;
  new_file->inode->i_fop = &vtfs_file_ops;

  list_add(&new_file->list, &parent_dir->children);
  d_add(child_dentry, new_file->inode);
  return 0;
}

int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry) {
  struct vtfs_dir* parent_dir;
  struct vtfs_file* file_entry;
  const char* name;

  LOG("Entering vtfs_unlink\n");

  if (!parent_inode || !child_dentry) {
    LOG("Invalid args");
    return -EINVAL;
  }

  parent_dir = parent_inode->i_private;
  if (!parent_dir) {
    LOG("Parent inode private data is NULL\n");
    return -EFAULT;
  }

  name = child_dentry->d_name.name;
  LOG("Attempting to unlink file: %s\n", name);

  list_for_each_entry(file_entry, &parent_dir->children, list) {
    if (strcmp(file_entry->name, name) == 0) {
      list_del(&file_entry->list);
      LOG("File %s removed from list\n", name);
      kfree(file_entry->name);
      kfree(file_entry->data);
      kfree(file_entry);

      inode_dec_link_count(child_dentry->d_inode);
      d_drop(child_dentry);

      LOG("File %s unlinked\n", name);
      return 0;
    }
  }

  LOG("File %s not found\n", name);
  return -ENOENT;
}

int vtfs_link(struct dentry* old_dentry, struct inode* parent_inode, struct dentry* new_dentry) {
  struct vtfs_file* old_file;
  struct vtfs_dir* parent_dir;
  struct vtfs_file* new_file;

  old_file = old_dentry->d_inode->i_private;
  parent_dir = parent_inode->i_private;

  if (S_ISDIR(old_file->mode)) {
    LOG("Hard links to directories are not allowed\n");
    return -EPERM;
  }

  struct list_head* pos;
  list_for_each(pos, &parent_dir->children) {
    struct vtfs_file* entry = list_entry(pos, struct vtfs_file, list);
    if (strcmp(entry->name, new_dentry->d_name.name) == 0) {
      LOG("File with the same name already exists: %s\n", new_dentry->d_name.name);
      return -EEXIST;
    }
  }

  new_file = kzalloc(sizeof(struct vtfs_file), GFP_KERNEL);
  if (!new_file) {
    LOG("kzalloc failed\n");
    return -ENOMEM;
  }

  new_file->name = kstrdup(new_dentry->d_name.name, GFP_KERNEL);
  new_file->size = old_file->size;
  new_file->data = old_file->data;
  new_file->inode = old_dentry->d_inode;

  list_add_tail(&new_file->list, &parent_dir->children);

  d_add(new_dentry, old_dentry->d_inode);

  inode_inc_link_count(old_dentry->d_inode);

  LOG("Hard link created\n");
  return 0;
}

int vtfs_iterate(struct file* flip, struct dir_context* ctx) {
  struct vtfs_dir* dir = flip->f_inode->i_private;
  struct list_head* pos;
  unsigned long offset = ctx->pos;
  unsigned long index = 0;

  list_for_each(pos, &dir->children) {
    if (index++ < offset)
      continue;

    struct vtfs_file* entry = list_entry(pos, struct vtfs_file, list);
    if (!dir_emit(
            ctx,
            entry->name,
            strlen(entry->name),
            entry->ino,
            S_ISDIR(entry->mode) ? DT_DIR : DT_REG
        )) {
      return -ENOMEM;
    }
    ctx->pos++;
  }

  return 0;
}

struct dentry* vtfs_lookup(
    struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag
) {
  struct vtfs_dir* parent_dir = parent_inode->i_private;

  struct list_head* pos;
  list_for_each(pos, &parent_dir->children) {
    struct vtfs_file* entry = list_entry(pos, struct vtfs_file, list);
    if (strcmp(entry->name, child_dentry->d_name.name) == 0) {
      d_add(child_dentry, entry->inode);
      return NULL;
    }
  }

  return NULL;
}

int vtfs_mkdir(
    struct mnt_idmap* idmap, struct inode* parent_inode, struct dentry* child_dentry, umode_t mode
) {
  struct vtfs_dir* parent_dir;
  struct vtfs_dir* new_dir;
  struct vtfs_file* new_file;

  if (!parent_inode || !child_dentry) {
    LOG("Invalid args\n");
    return -EINVAL;
  }

  parent_dir = parent_inode->i_private;
  if (!parent_dir) {
    LOG("Parent dir is NULL\n");
    return -EFAULT;
  }

  new_file = kzalloc(sizeof(*new_file), GFP_KERNEL);
  if (!new_file) {
    LOG("kzalloc failed file\n");
    return -ENOMEM;
  }

  new_file->name = kstrdup(child_dentry->d_name.name, GFP_KERNEL);
  if (!new_file->name) {
    kfree(new_file);
    return -ENOMEM;
  }

  new_dir = kzalloc(sizeof(*new_dir), GFP_KERNEL);
  if (!new_dir) {
    LOG("kzalloc failed dir\n");
    kfree(new_file->name);
    kfree(new_file);
    return -ENOMEM;
  }

  INIT_LIST_HEAD(&new_dir->children);
  new_dir->self = new_file;

  new_file->ino = get_next_ino();
  new_file->mode = S_IFDIR | mode;
  new_file->size = 0;
  new_file->data = NULL;
  new_file->inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, new_file->mode, new_file->ino);
  new_file->inode->i_private = new_dir;
  new_file->inode->i_op = &vtfs_inode_ops;
  new_file->inode->i_fop = &vtfs_dir_ops;
  list_add_tail(&new_file->list, &parent_dir->children);

  d_add(child_dentry, new_file->inode);

  LOG("Dir %s created\n", child_dentry->d_name.name);
  return 0;
}

int vtfs_rmdir(struct inode* parent_inode, struct dentry* child_dentry) {
  struct vtfs_dir* parent_dir;
  struct vtfs_dir* target_dir;
  struct vtfs_file* target_file;
  struct inode* target_inode;

  if (!parent_inode || !child_dentry) {
    LOG("Invalid args\n");
    return -EINVAL;
  }

  parent_dir = parent_inode->i_private;
  target_inode = child_dentry->d_inode;

  if (!parent_dir || !target_inode) {
    LOG("Invalid parent or inode\n");
    return -EFAULT;
  }

  target_dir = target_inode->i_private;

  if (!target_dir || !target_dir->self) {
    LOG("Dir corrupted\n");
    return -EFAULT;
  }

  target_file = target_dir->self;

  if (!list_empty(&target_dir->children)) {
    LOG("Directory %s is not empty\n", child_dentry->d_name.name);
    return -ENOTEMPTY;
  }

  list_del_init(&target_file->list);

  inode_dec_link_count(target_inode);
  d_drop(child_dentry);

  kfree(target_file->name);
  kfree(target_file);
  kfree(target_dir);
  LOG("Dir %s removed\n", child_dentry->d_name.name);
  return 0;
}

struct inode* vtfs_get_inode(
    struct super_block* sb, const struct inode* dir, umode_t mode, int i_ino
) {
  struct inode* inode = new_inode(sb);
  struct mnt_idmap* idmap = &nop_mnt_idmap;
  if (inode != NULL) {
    inode_init_owner(idmap, inode, dir, mode);
    inode->i_mode = mode;
  }
  inode->i_ino = i_ino;
  return inode;
}

int vtfs_fill_super(struct super_block* sb, void* data, int silent) {
  struct vtfs_dir* root_dir;
  struct vtfs_file* root_file;

  root_dir = kmalloc(sizeof(struct vtfs_dir), GFP_KERNEL);
  if (!root_dir) {
    return -ENOMEM;
  }

  INIT_LIST_HEAD(&root_dir->children);

  root_file = kmalloc(sizeof(struct vtfs_file), GFP_KERNEL);
  if (!root_file) {
    kfree(root_dir);
    return -ENOMEM;
  }

  root_file->name = kstrdup("/", GFP_KERNEL);
  root_file->ino = 100;
  root_file->mode = S_IFDIR | 0777;
  root_file->inode = vtfs_get_inode(sb, NULL, root_file->mode, root_file->ino);
  root_file->data = NULL;
  root_file->size = 0;

  root_dir->self = root_file;

  root_file->inode->i_private = root_dir;
  root_file->inode->i_op = &vtfs_inode_ops;
  root_file->inode->i_fop = &vtfs_dir_ops;

  sb->s_root = d_make_root(root_file->inode);
  if (!sb->s_root) {
    kfree(root_file->name);
    kfree(root_file);
    kfree(root_dir);
    return -ENOMEM;
  }

  LOG("Superblock initialized");
  return 0;
}

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "vtfs super block is destroyed. Unmount successfully.\n");
}

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb,
};

struct dentry* vtfs_mount(
    struct file_system_type* fs_type, int flags, const char* token, void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    printk(KERN_ERR "Can't mount file system");
  } else {
    printk(KERN_INFO "Mounted successfully");
  }
  return ret;
}

static int __init vtfs_init(void) {
  register_filesystem(&vtfs_fs_type);
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  unregister_filesystem(&vtfs_fs_type);
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
