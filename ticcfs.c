/*
TODO: 
 * umožněte připojení více souborových systémů najednou (mount
   options, sb->s_fs_info (private SB data), sb->s_options,
   (generic_)show_options(), mount -t posfs none /kam -o A)
 * přidejte podporu pro podadresáře
 - umožněte neomezený počet objektů v souborovém systému (čím je reálně omezeno?) - 2^(sizeof(unsigned long))
 - data souboru ukládejte do samostatné stránky, v i-node bude jen odkaz na stránku (max. datasize 4KB)
 - rozšiřte ukládání souborů na systém s N přímými dat. bloky, M nepřímými


predelat statfs
*/
#include "ticcfs.h"
#include <linux/module.h>
//#include <linux/config.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/statfs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lukas Zilka <lukas@zilka.me>");

static struct kmem_cache * ticcfs_inode_cachep;

int ticcfs_get_page_number_of(loff_t size)
{
        return size / PAGE_SIZE + ((size % PAGE_SIZE) > 0);
}

void ticcfs_get_direct_indirect(loff_t size, int *nr_indirect, int *nr_total_indirect, int *nr_direct)
{
        int nr_pages = ticcfs_get_page_number_of(size);
        *nr_direct = nr_pages;

        if(*nr_direct > TICCFS_NDIR) {
                DEBUGS("have to use indirect");
                *nr_direct = TICCFS_NDIR;
                *nr_total_indirect = (nr_pages - TICCFS_NDIR);
                *nr_indirect = *nr_total_indirect / TICCFS_NDIR_PER_PAGE + 
                        (*nr_total_indirect % TICCFS_NDIR_PER_PAGE > 0);
        } else {
                *nr_total_indirect = 0;
                *nr_indirect = 0;
        }
        
}

long unsigned min_l(long unsigned a, long unsigned b) {
        if(a > b)
                return b;
        else
                return a;
}

static void ticcfs_realloc_data(struct inode * inode, loff_t size)
{       
        int i, y;
        int nr_pages = ticcfs_get_page_number_of(size);
        int nr_direct = nr_pages;
        int nr_indirect = 0;
        int nr_total_indirect = 0;

        DEBUGM("size: %lld, page_size: %ld", (long long) size, PAGE_SIZE);

        ticcfs_get_direct_indirect(size, &nr_indirect, &nr_total_indirect, &nr_direct);

        DEBUGM("reallocing to %d direct and %d indirect (%d total indirect)", nr_direct, nr_indirect, nr_total_indirect);
        
        for(i = 0; i < nr_direct; i++) {
                DEBUGS("reallocing direct");
                if(TICCFS_I(inode)->data_direct[i] == NULL) {
                        TICCFS_I(inode)->data_direct[i] = (void *) get_zeroed_page(GFP_KERNEL);
                        DEBUGM("realloced %p", TICCFS_I(inode)->data_direct[i]);
                } else {
                        DEBUGM("not realloced %p", TICCFS_I(inode)->data_direct[i]);
                }
        }

        for(i = 0; i < nr_indirect; i++) {
                DEBUGS("reallocing indirect");
                if(TICCFS_I(inode)->data_indirect[i] == NULL) {
                        TICCFS_I(inode)->data_indirect[i] = (char **) get_zeroed_page(GFP_KERNEL);                        
                        DEBUGM("realloced %p", TICCFS_I(inode)->data_indirect[i]);
                } else {
                        DEBUGM("not realloced %p", TICCFS_I(inode)->data_indirect[i]);
                }
                DEBUGM(" ===== min %lu %lu = %lu", nr_total_indirect, TICCFS_NDIR_PER_PAGE, min_l(nr_total_indirect, TICCFS_NDIR_PER_PAGE));
                for(y = 0; y < min_l(nr_total_indirect, TICCFS_NDIR_PER_PAGE); y++) {                                
                        if(TICCFS_I(inode)->data_indirect[i][y] == NULL) {
                                TICCFS_I(inode)->data_indirect[i][y] = (void *) get_zeroed_page(GFP_KERNEL);                                                                
                                DEBUGM("realloced indirect2 %p", TICCFS_I(inode)->data_indirect[i][y]);
                        } else {
                                DEBUGM("not realloced indirect2 %p", TICCFS_I(inode)->data_indirect[i][y]);
                        }
                }
                nr_total_indirect -= y;
        }


}

static int ticcfs_parse_options(char *data, struct ticcfs_mount_opts *opts) 
{
        DEBUG();

        if(data != NULL && strlen(data) > 0) 
        {
                opts->id = data[0];
        }
        else 
        {
                opts->id = TICCFS_DEFAULT_ID;
        }
        return 0;
}

static struct inode *ticcfs_alloc_inode(struct super_block *sb)
{
        struct ticcfs_inode *ti;

        DEBUG();

        ti = (struct ticcfs_inode *) kmem_cache_alloc(ticcfs_inode_cachep, GFP_KERNEL);        
        if(!ti)
                return NULL;

        return &ti->vfs_inode;
}


static int ticcfs_readdir(struct file * filp, void *dirent, filldir_t filldir) 
{
	struct dentry *dentry = filp->f_path.dentry;
        ino_t ino;
	int i = filp->f_pos;
        struct inode *inode;
        struct ticcfs_dir *d;

        DEBUG();

        inode = dentry->d_inode;
        d = (struct ticcfs_dir *) TICCFS_I(inode)->data_direct[0];
        
	switch (i) {
		case 0:
			ino = dentry->d_inode->i_ino;
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
				break;
			filp->f_pos++;
			i++;
			/* fallthrough */
		case 1:
			ino = parent_ino(dentry);
			if (filldir(dirent, "..", 2, i, ino, DT_DIR) < 0)
				break;
			filp->f_pos++;
			i++;
			/* fallthrough */
		default:
                        DEBUGS("default");
                        for(; i - 2 < d->cnt; i++) 
                        {
                                DEBUGM("iter #%d/%d", i-2, d->cnt);
                                if(filldir(dirent, d->entries[i - 2].name,
                                           strlen(d->entries[i - 2].name),
                                           filp->f_pos,
                                           d->entries[i - 2].i->i_ino,
                                           d->entries[i - 2].type) < 0) {
                                        DEBUGS("iter <0");
                                        return 0;
                                }
                                filp->f_pos++;
                        }
                        DEBUGS("default end");

        }
        return 0;
}

static void *ticcfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
        nd_set_link(nd, TICCFS_I(dentry->d_inode)->data_direct[0]);

	return NULL;
}

int ticcfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		   struct kstat *stat)
{
        printk("ticcfs: getattr\n");
        return simple_getattr(mnt, dentry, stat);
}


loff_t ticcfs_llseek(struct file * f, loff_t o, int x)
{
        return o;
}

char * ticcfs_data_page(struct inode * inode, loff_t offset, size_t *max_write)
{
        DEBUG();
        int nr_direct, nr_indirect, nr_total_indirect;
        char *res;

        *max_write = PAGE_SIZE - (offset % PAGE_SIZE);
        ticcfs_get_direct_indirect(offset + 1, &nr_indirect, &nr_total_indirect, &nr_direct);

        DEBUGM("offset: %ld have direct %d indirect %d", (long)offset, nr_direct, nr_indirect);
        
        if(nr_indirect == 0) {
                res = TICCFS_I(inode)->data_direct[nr_direct - 1];
                res += offset % PAGE_SIZE;
        } else {
                DEBUGM("indirect: %d, %%indirect %ld", nr_indirect, (nr_total_indirect % TICCFS_NDIR_PER_PAGE));
                res = *(TICCFS_I(inode)->data_indirect[nr_indirect - 1] + ((nr_total_indirect % TICCFS_NDIR_PER_PAGE) - 1));
                res += offset % PAGE_SIZE;
                DEBUGS("got pointer");
        }

        DEBUGM("returning %p", res);

        return res;
}

ssize_t ticcfs_read(struct file * f, char __user * buff, size_t buff_len, loff_t * ppos)
{
        struct dentry *dentry = f->f_dentry;        
        struct inode *inode = dentry->d_inode;
        char * d;
        size_t remains_to_read = buff_len;
        size_t max_read;
        size_t read_cnt = 0;

        if(*ppos < inode->i_size) {
                while(remains_to_read > 0) {
                        d = ticcfs_data_page(inode, *ppos, &max_read);
                        if(max_read > remains_to_read)
                                max_read = remains_to_read;
                        
                        DEBUGM("reading %ld, %ld remains", max_read, remains_to_read);
                        copy_to_user(buff, d, max_read);
                
                        *ppos += max_read;
                        read_cnt += max_read;
                        remains_to_read -= max_read;
                }
                DEBUGM("returing that we read %ld", read_cnt);
                return read_cnt;
        } else {
                return 0;
        }


/*
        struct dentry *dentry = f->f_dentry;

        
        struct inode *inode = dentry->d_inode;
        struct ticcfs_inode *ei = TICCFS_I(inode);

        DEBUGM("read %d", (int)*ppos);        
        
        DEBUGM("size of this file is %d", (int) inode->i_size);
        if(*ppos <= inode->i_size) {
                int how_much_read = buff_len;
                if(buff_len > inode->i_size - *ppos) {
                        how_much_read = inode->i_size - *ppos;
                }
                copy_to_user(buff, ei->data_direct[0] + *ppos, how_much_read);
                *ppos += how_much_read;

                DEBUGM("%d read", how_much_read);

                return how_much_read;
        }
        else {
                return 0;
                }*/
}

ssize_t ticcfs_write(struct file * f, const char __user * buff, size_t buff_size, loff_t * offset)
{
        struct dentry *dentry = f->f_dentry;        
        struct inode *inode = dentry->d_inode;
        char * d;
        size_t remains_to_write = buff_size;
        size_t max_write;
        size_t write_cnt = 0;

        ticcfs_realloc_data(inode, *offset + buff_size);

        while(remains_to_write > 0) {
                d = ticcfs_data_page(inode, *offset, &max_write);
                if(max_write > remains_to_write)
                        max_write = remains_to_write;

                copy_from_user(d, buff, max_write);
                
                inode->i_size = *offset + max_write;
                *offset += max_write;
                write_cnt += max_write;
                remains_to_write -= max_write;
        }
        
        DEBUGM("written %ld", write_cnt);

        return write_cnt;
}

int ticcfs_release(struct inode * inode, struct file * f)
{
        return 0;
}

static struct inode* ticcfs_get_inode(struct super_block* sb, umode_t mode, dev_t dev) 
{
        struct inode* inode;
        struct ticcfs_dir *d;

        DEBUG();

        inode = new_inode(sb);
        DEBUGM("sb %p imapping-aops: %p", TICCFS_I(inode)->vfs_inode.i_sb, inode->i_mapping->a_ops);
    
        if(!inode)
                return NULL;

        inode->i_mode = mode;
        inode->i_uid = 0;
        inode->i_gid = 0;
        inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
        mark_inode_dirty(inode);


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
                TICCFS_I(inode)->data_direct[0] = (void *) get_zeroed_page(GFP_KERNEL); // alloc_page(GFP_KERNEL);

                d = (struct ticcfs_dir *) TICCFS_I(inode)->data_direct[0];
                d->cnt = 0;

                inode->i_fop = &ticcfs_dir_operations;
                inode->i_op = &ticcfs_dir_inode_operations; // be careful to assign this property only to directory entries (otherwise the VFS will consider this inode to be a directory)
                DEBUGM("%s", "before nlink");
                inc_nlink(inode);
                DEBUGM("%s", "after nlink");
                break;
        case S_IFLNK:
                TICCFS_I(inode)->data_direct[0] = (void *) get_zeroed_page(GFP_KERNEL); // alloc_page(GFP_KERNEL);
                inode->i_op = &ticcfs_file_inode_operations;
                break;
        }

        return inode;
}

static int ticcfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
        struct ticcfs_dir *d;
        struct inode * inode;

        DEBUG();
        inode = ticcfs_get_inode(dir->i_sb, mode, dev);
        if(inode) 
        {
                DEBUGM("%s %p %p %lu", dentry->d_name.name, inode->i_op, &ticcfs_file_inode_operations, inode->i_ino);
                d = (struct ticcfs_dir *) TICCFS_I(dir)->data_direct[0];
                DEBUGM("# of entries %d", d->cnt);
                d->entries[d->cnt].i = inode;
                strncpy(d->entries[d->cnt].name, dentry->d_name.name, dentry->d_name.len);
                d->cnt += 1;

                d_instantiate(dentry, inode);
                dget(dentry);
                inode->i_gid = dir->i_gid;
                dir->i_mtime = dir->i_ctime = CURRENT_TIME;

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

        DEBUG();

        if(!retval)
                inc_nlink(dir);
        return retval;
}

static int ticcfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd) 
{
        DEBUG();

        return ticcfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int ticcfs_symlink(struct inode *dir, struct dentry *dentry, const char * symname) 
{
        struct inode *inode;

        DEBUG();

        inode = ticcfs_get_inode(dir->i_sb, S_IFLNK | S_IRWXUGO, 0);
        if(inode) 
        {
                d_instantiate(dentry, inode);
                dget(dentry);
                dir->i_mtime = dir->i_ctime = CURRENT_TIME;

                printk("ticcfs: new symlink %s\n", symname);
                strcpy(TICCFS_I(inode)->data_direct[0], symname);
                inode->i_size = strlen(symname) + 1;

                return 0;
        }

        return -ENOSPC;
}

struct dentry *ticcfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
        DEBUG();
        
        return simple_lookup(dentry->d_inode, dentry, nd);
}


int ticcfs_fill_super(struct super_block* sb, void* data, int silent) 
{
        struct dentry* root;
        struct inode* inode;
        struct ticcfs_fs_info *fsi;

        DEBUG();

        save_mount_options(sb, data);

        sb->s_magic = TICCFS_MAGIC;
        sb->s_op = &ticcfs_sops;
        
        fsi = kzalloc(sizeof(struct ticcfs_fs_info), GFP_KERNEL);
        sb->s_fs_info = fsi;
        
        ticcfs_parse_options(data, &fsi->mount_opts);
        
        inode = ticcfs_get_inode(sb, S_IFDIR | 0755, 0);
        if(!inode) return -ENOMEM;

        DEBUGM("d_alloc_root %lu", inode->i_ino);
        root = d_alloc_root(inode);
        DEBUGS("after alloc root");
        if(!root) return -ENOMEM;

        sb->s_root = root;
        DEBUGS("end fill super");
        return 0;
}

char ticcfs_get_id(char *data) 
{
        DEBUG();

        if(data == NULL || strlen(data) == 0)
                return TICCFS_DEFAULT_ID;
        else {
                return data[0];
        }
}

int ticcfs_get_sb(struct file_system_type* fstype, int flags, const char* dev_name, 
                  void* data, struct vfsmount* mnt) 
{
        struct ticcfs_fs_info *fsi;
        struct super_block *s;
        char id = ticcfs_get_id(data);
        
        DEBUG();

        // search for already active ticcfs superblocks with the same
        // id so that we can mount the existing structure to the
        //current dest
        list_for_each_entry(s, &(fstype->fs_supers), s_instances) {        
                fsi = (struct ticcfs_fs_info *)s->s_fs_info;
                if(fsi->mount_opts.id == id) {
                        printk("ticcfs: reusing\n");
                        simple_set_mnt(mnt, s);
                        return 0;
                }
        }
        
        // create sb, if no active ticcfs superblock with this id exists
        return get_sb_nodev(fstype, flags, data, ticcfs_fill_super, mnt);
}

static void init_once(void *foo) 
{
        struct ticcfs_inode *ei = (struct ticcfs_inode *) foo;
        inode_init_once(&ei->vfs_inode);
}

static int ticcfs_init_inodecache(void)
{
        DEBUG();

	ticcfs_inode_cachep = kmem_cache_create("ticcfs_inode_cache",
					     sizeof(struct ticcfs_inode),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (ticcfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void ticcfs_destroy_inodecache(void)
{
        DEBUG();

	kmem_cache_destroy(ticcfs_inode_cachep);
}


void ticcfs_kill_sb(struct super_block* sb) 
{
        DEBUG();

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
        ticcfs_init_inodecache();
        register_filesystem(&fs_type);
        return 0; 
}

static void __exit ticcfs_exit(void) {
        unregister_filesystem(&fs_type);
        ticcfs_destroy_inodecache();
}

module_init(ticcfs_init);
module_exit(ticcfs_exit);
