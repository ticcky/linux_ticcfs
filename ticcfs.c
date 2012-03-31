#include <linux/module.h>
//#include <linux/config.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/statfs.h>
#include <linux/namei.h>


MODULE_LICENSE("GPL");

#define TICCFS_MAGIC 0x99999
#define TICCFS_COUNT 32  // max number of files
#define TICCFS_NAMELEN 64 // max filename size
#define TICCFS_DATALEN 32 // max filesize

typedef struct {
        struct inode*   i;
        char            name[TICCFS_NAMELEN];
        char            data[TICCFS_DATALEN];
        int             datalen;
} t_tfs_direntry;

// this is where the information about objects (files/dirs in our
// filesystem is going to be stored
static t_tfs_direntry tfs_direntry[TICCFS_COUNT];
static int tfs_cnt = 0;

const struct inode_operations ticcfs_dir_inode_operations;

struct ticcfs_inode_info {
        struct inode vfs_inode;
};

static struct super_operations ticcfs_sops = {
        .statfs = simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
};

const struct file_operations ticcfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.readdir	= dcache_readdir,
	.fsync		= simple_sync_file,
};

t_tfs_direntry *get_tfs_direntry(struct inode *inode) 
{
        int i;
        for(i = 0; i < tfs_cnt; i++)
        {
                if(inode == tfs_direntry[i].i)
                        return &(tfs_direntry[i]);
        } 
        return NULL;
}

t_tfs_direntry *get_tfs_direntry_by_name(struct qstr *name) 
{
        int i;
        for(i = 0; i < tfs_cnt; i++)
        {
                if(memcmp(tfs_direntry[i].name, name->name, name->len) == 0)
                        return &(tfs_direntry[i]);
        } 
        return NULL;
}


static void *ticcfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
        t_tfs_direntry *di = get_tfs_direntry(dentry->d_inode);
        if(di != NULL) {
                nd_set_link(nd, di->data);
        } else {
                nd_set_link(nd, "nic");
        }

	return NULL;
}

int ticcfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		   struct kstat *stat)
{
        printk("ticcfs: getattr\n");
        return simple_getattr(mnt, dentry, stat);
}

const struct inode_operations ticcfs_file_inode_operations = {
	.getattr	= ticcfs_getattr,
	.readlink	= generic_readlink,
        .follow_link    = ticcfs_follow_link,
};

loff_t ticcfs_llseek(struct file * f, loff_t o, int x)
{
        return o;
}

ssize_t ticcfs_read(struct file * f, char __user * buff, size_t buff_len, loff_t * ppos)
{
        printk("ticcfs: read %d\n", (int)*ppos);        
        struct dentry *dentry = f->f_dentry;
        
        struct inode *inode = dentry->d_inode;
        t_tfs_direntry *de = get_tfs_direntry(inode);
        
        printk("ticcfs: size of this file is %d\n", de->datalen);
        if(*ppos <= de->datalen) {
                int how_much_read = buff_len;
                if(buff_len > de->datalen - *ppos) {
                        how_much_read = de->datalen - *ppos;
                }
                copy_to_user(buff, de->data + *ppos, how_much_read);
                *ppos += how_much_read;

                printk("ticcfs: %d read\n", how_much_read);
                return how_much_read;
        }
        else {
                return 0;
        }
}

ssize_t ticcfs_write(struct file * f, const char __user * buff, size_t buff_size, loff_t * offset)
{
        printk("ticcfs: write %d\n", (int)*offset);        
        struct dentry *dentry = f->f_dentry;        
        struct inode *inode = dentry->d_inode;
        t_tfs_direntry *de = get_tfs_direntry(inode);
        
        if(*offset < TICCFS_DATALEN) {
                int how_much_write = TICCFS_DATALEN - *offset;
                if(buff_size < how_much_write)
                        how_much_write = buff_size;

                copy_from_user(de->data + *offset, buff, how_much_write);
                de->datalen = *offset + how_much_write;
                *offset += how_much_write;
                printk("ticcfs: %d written\n", how_much_write);
                return how_much_write;
        }
        else {
                return buff_size;
        }
}

int ticcfs_release(struct inode * inode, struct file * f)
{
        return 0;
}

const struct file_operations ticcfs_file_operations = {
        .llseek         = ticcfs_llseek,
        .read           = ticcfs_read,
        .write          = ticcfs_write,
        .release        = ticcfs_release,
};

static struct inode* ticcfs_get_inode(struct super_block* sb, umode_t mode, dev_t dev) 
{
        printk("ticcfs: get inode\n");
        struct inode* inode;

        if(tfs_cnt >= TICCFS_COUNT)
                return NULL;

        inode = new_inode(sb);
        
        if(!inode)
                return NULL;

        inode->i_mode = mode;
        inode->i_uid = 0;
        inode->i_gid = 0;
        inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
        printk("ticcfs: new inode %lu\n", inode->i_ino);

        printk("ticcfs: %d | %d %d\n", mode, S_IFREG, S_IFDIR);

        switch (mode & S_IFMT)
        {
        default:
                init_special_inode(inode, mode, dev);
                break;
        case S_IFREG:
                printk("ticcfs: file\n");
                inode->i_fop = &ticcfs_file_operations;
                break;
        case S_IFDIR:             
                printk("ticcfs: dir\n");
                inode->i_fop = &ticcfs_dir_operations;
                inode->i_op = &ticcfs_dir_inode_operations; // be careful to assign this property only to directory entries (otherwise the VFS will consider this inode to be a directory)

                inc_nlink(inode);
                break;
        case S_IFLNK:
                inode->i_op = &ticcfs_file_inode_operations;
                break;
        }

        return inode;
}

static int ticcfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
        struct inode * inode = ticcfs_get_inode(dir->i_sb, mode, dev);
        if(inode) 
        {
                printk("ticcfs: mknod %s %p %p %lu\n", dentry->d_name.name, inode->i_op, &ticcfs_file_inode_operations, inode->i_ino);
                d_instantiate(dentry, inode);
                dget(dentry);
                inode->i_gid = dir->i_gid;
                dir->i_mtime = dir->i_ctime = CURRENT_TIME;

                tfs_direntry[tfs_cnt].i = inode;
                tfs_cnt += 1;

                return 0;
        }
        else 
        {
                return -ENOSPC;
        }
}

static int ticcfs_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
        int retval = ticcfs_mknod(dir, dentry, mode | S_IFDIR, 0);
        if(!retval)
                inc_nlink(dir);
        return retval;
}

static int ticcfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd) 
{
        return ticcfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int ticcfs_symlink(struct inode *dir, struct dentry *dentry, const char * symname) 
{
        struct inode *inode;
        inode = ticcfs_get_inode(dir->i_sb, S_IFLNK | S_IRWXUGO, 0);
        if(inode) 
        {
                d_instantiate(dentry, inode);
                dget(dentry);
                dir->i_mtime = dir->i_ctime = CURRENT_TIME;

                printk("ticcfs: new symlink %s\n", symname);
                tfs_direntry[tfs_cnt].i = inode;
                strcpy(tfs_direntry[tfs_cnt].data, symname);
                tfs_direntry[tfs_cnt].datalen = strlen(symname) + 1;
                tfs_cnt += 1;


                return 0;
        }

        return -ENOSPC;
}

struct dentry *ticcfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
        return simple_lookup(dentry->d_inode, dentry, nd);
}


const struct inode_operations ticcfs_dir_inode_operations = {
	.lookup		= ticcfs_lookup,
	.mknod		= ticcfs_mknod,
        .mkdir          = ticcfs_mkdir,
        .symlink        = ticcfs_symlink,
        .create         = ticcfs_create,
};

int ticcfs_fill_super(struct super_block* sb, void* data, int silent) 
{
        struct dentry* root;
        struct inode* inode;

        sb->s_magic = TICCFS_MAGIC;
        sb->s_op = &ticcfs_sops;
        
        inode = ticcfs_get_inode(sb, S_IFDIR | 0755, 0);
        if(!inode) return -ENOMEM;

        root = d_alloc_root(inode);
        if(!root) return -ENOMEM;

        sb->s_root = root;

        return 0;
}

int ticcfs_get_sb(struct file_system_type* fstype, int flags, const char* dev_name, 
                  void* data, struct vfsmount* mnt) 
{
        return get_sb_nodev(fstype, flags, data, ticcfs_fill_super, mnt);
}

void ticcfs_kill_sb(struct super_block* sb) 
{
        kill_litter_super(sb);
}

static struct file_system_type fs_type = 
{ 
        .name = "ticcfs",
        .fs_flags = 0,
        .get_sb = ticcfs_get_sb,
        .kill_sb = ticcfs_kill_sb,
        .owner = THIS_MODULE,
        .next = NULL
};

static int __init ticcfs_init(void) {
        register_filesystem(&fs_type);
        return 0; 
}

static void __exit ticcfs_exit(void) {
        unregister_filesystem(&fs_type);
}

module_init(ticcfs_init);
module_exit(ticcfs_exit);
