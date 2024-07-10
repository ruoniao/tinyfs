#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
 
#include "tinyfs.h"
 
#define tinyfs_dbg pr_debug
#define tinyfs_err pr_err
 
struct file_blk block[MAX_FILES+1];
 
int curr_count = 0;
 
static int get_block(void)
{
    int i;
    for( i = 2; i < MAX_FILES; i++) {
        if (!block[i].busy) {
            block[i].busy = 1;
            return i;
        }
    }
 
    return -1;
}
 
static struct inode_operations tinyfs_inode_ops;
 
/*
 * read the entries from a directory
 */
static int tinyfs_readdir(struct file *filp, struct dir_context *ctx)
{
 
    loff_t pos;
    struct file_blk  *blk;
    struct dir_entry *entry;
    struct dentry *dentry = filp->f_path.dentry;
 
    int i;
 
    if (!dir_emit_dots(filp, ctx))
        return 0;
 
    pos = filp->f_pos;
 
    if (pos)
        return 0;
 
    blk = (struct file_blk*)dentry->d_inode->i_private;
 
    if (!S_ISDIR(blk->mode)) {
        return -ENOTDIR;
    }
 
    //loop get one dir included file name
    entry = (struct dir_entry *)blk->dir_data;
    for (i = 0; i < blk->dir_children; i++) {
        if (!dir_emit(ctx, entry[i].filename, strlen(entry[i].filename),
                    entry[i].idx,
                    DT_UNKNOWN))
            break;
        filp->f_pos += sizeof(struct dir_entry);
        pos += sizeof(struct dir_entry);
    }
 
    return 0;
}
 
ssize_t tinyfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct file_blk *blk;
    char *buffer;
 
    blk = (struct file_blk*)filp->f_path.dentry->d_inode->i_private;
 
    if (*ppos >= blk->file_size)
        return 0;
 
    buffer = (char *)blk->file_data;
    len = min((size_t)blk->file_size, len);
 
    if (copy_to_user(buf, buffer, len)) {
        return -EFAULT;
    }
    *ppos += len;
 
    return len;
}
 
ssize_t tinyfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
    struct file_blk *blk;
    char *buffer;
 
    blk = filp->f_path.dentry->d_inode->i_private;
 
    buffer = (char *)blk->file_data;
    buffer += *ppos;
 
    if (copy_from_user(buffer, buf, len)) {
        return -EFAULT;
    }
 
    *ppos += len;
    blk->file_size = *ppos;
 
    return len;
}
 
const struct file_operations tinyfs_file_operations = {
    .read = tinyfs_read,
    .write = tinyfs_write,
};
 
static const struct file_operations tinyfs_dir_operations = {
    .owner = THIS_MODULE,
    .read = generic_read_dir,
    .iterate_shared	= tinyfs_readdir,
};
 
// 函数声明，创建文件或目录
static int tinyfs_do_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct inode *inode;         // 新文件或目录的 inode
    struct super_block *sb;      // 超级块
    struct dir_entry *entry;     // 目录项
    struct file_blk *blk, *pblk; // 文件块和父目录的文件块
    int idx;                     // 块索引

    sb = dir->i_sb; // 获取父目录的超级块

    // 检查当前文件数是否超过最大文件数限制
    if (curr_count >= MAX_FILES) {
        return -ENOSPC; // 返回没有空间错误
    }

    // 检查模式是否为目录或常规文件
    if (!S_ISDIR(mode) && !S_ISREG(mode)) {
        return -EINVAL; // 返回无效参数错误
    }

    // 获取父目录的文件块
    pblk = (struct file_blk *)dir->i_private;

    // 检查父目录包含的文件或子目录数是否超过最大限制
    if (pblk->dir_children >= MAX_SUBDIR_FILES) {
        tinyfs_err("one dir max include file cnt should less than %d\n", MAX_SUBDIR_FILES);
        return -ENOSPC; // 返回没有空间错误
    }

    // 分配一个新的 inode
    inode = new_inode(sb);

    // 检查是否成功分配 inode
    if (!inode) {
        return -ENOMEM; // 返回内存不足错误
    }

    // 初始化 inode 的超级块、操作函数指针和时间戳
    inode->i_sb = sb;
    inode->i_op = &tinyfs_inode_ops;
    inode->i_atime = inode->i_mtime = inode_set_ctime_current(inode);

    // 获取一个空闲块的索引，用于存储新文件或目录
    idx = get_block();

    // 获取空闲块，并初始化块和 inode
    blk = &block[idx];
    inode->i_ino = idx;
    blk->mode = mode;
    curr_count++;

    // 根据模式设置块和 inode 的操作函数指针
    if (S_ISDIR(mode)) {
        blk->dir_children = 0;
        inode->i_fop = &tinyfs_dir_operations; // 设置目录操作函数
    } else if (S_ISREG(mode)) {
        blk->file_size = 0;
        inode->i_fop = &tinyfs_file_operations; // 设置文件操作函数
    }

    // 将块指针存储到 inode 的私有数据中
    inode->i_private = blk;

    // 打印调试信息，显示函数名和父目录的 inode
    tinyfs_dbg("%s dir inode %llx", __func__, dir);

    // 获取父目录的目录项，并增加子项计数
    entry = &pblk->dir_data[pblk->dir_children];
    pblk->dir_children++;

    // 设置新目录项的索引和文件名
    entry->idx = idx;
    // 注意：此时的entry指向的是pblk 父级目录的block的最后dir_data，操作父级目录记录该创建目录的文件名，在ls 时才能看到子目录
    strncpy(entry->filename, dentry->d_name.name, MAXLEN-1);
    entry->filename[MAXLEN-1] = 0; // 确保字符串以 '\0' 结尾


    // 初始化 inode 的所有者，并将 inode 添加到目录项中，linux 中的内核操作
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    d_add(dentry, inode);

    // 返回 0，表示成功创建文件或目录
    return 0;
}

 
static int tinyfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
    return tinyfs_do_create(&nop_mnt_idmap, dir, dentry, S_IFDIR | mode);
}
 
static int tinyfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
    return tinyfs_do_create(&nop_mnt_idmap, dir, dentry, mode);
}
 
static struct inode * tinyfs_iget(struct super_block *sb, int idx)
{
    struct inode *inode;
    struct file_blk *blk;
 
    inode = new_inode(sb);
    inode->i_ino = idx;
    inode->i_sb = sb;
    inode->i_op = &tinyfs_inode_ops;
 
    blk = &block[idx];
 
    if (S_ISDIR(blk->mode))
        inode->i_fop = &tinyfs_dir_operations;
    else if (S_ISREG(blk->mode))
        inode->i_fop = &tinyfs_file_operations;
 
    inode->i_atime = inode->i_mtime = inode_set_ctime_current(inode);
    inode->i_private = blk;
 
    return inode;
}
 
struct dentry *tinyfs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags)
{
    struct super_block *sb = parent_inode->i_sb;
    struct file_blk *blk;
    struct dir_entry *entry;
    int i;
 
    blk = (struct file_blk *)parent_inode->i_private;
    entry = (struct dir_entry *)blk->dir_data;
    for (i = 0; i < blk->dir_children; i++) {
        if (!strcmp(entry[i].filename, child_dentry->d_name.name)) {
            struct inode *inode = tinyfs_iget(sb, entry[i].idx);
            struct file_blk *inner = (struct file_blk*)inode->i_private;
            inode_init_owner(&nop_mnt_idmap, inode, parent_inode, inner->mode);
            d_add(child_dentry, inode);
            return NULL;
        }
    }
 
    return NULL;
}
 
int tinyfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = dentry->d_inode;
    struct file_blk *blk = (struct file_blk*) inode->i_private;
 
    blk->busy = 0;
    return simple_rmdir(dir, dentry);
}
 
int tinyfs_unlink(struct inode *dir, struct dentry *dentry)
{
    int i;
    struct inode *inode = dentry->d_inode;
    struct file_blk *blk = (struct file_blk *)inode->i_private;
    struct file_blk *pblk = (struct file_blk *)dir->i_private;
    struct dir_entry *entry;
 
    entry = (struct dir_entry*)pblk->dir_data;
    for (i = 0; i < pblk->dir_children; i++) {
        if (!strcmp(entry[i].filename, dentry->d_name.name)) {
            int j;
            for (j = i; j < pblk->dir_children - 1; j++) {
                memcpy(&entry[j], &entry[j+1], sizeof(struct dir_entry));
            }
            pblk->dir_children --;
            break;
        }
    }
 
    blk->busy = 0;
    return simple_unlink(dir, dentry);
}
 
static struct inode_operations tinyfs_inode_ops = {
    .create = tinyfs_create,
    .lookup = tinyfs_lookup,
    .mkdir = tinyfs_mkdir,
    .rmdir = tinyfs_rmdir,
    .unlink = tinyfs_unlink,
};
 
int tinyfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *root_inode;
    umode_t mode = S_IFDIR;
 
    root_inode = new_inode(sb);
    root_inode->i_ino = 1;
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, mode);
    root_inode->i_sb = sb;
    root_inode->i_op = &tinyfs_inode_ops;
    root_inode->i_fop = &tinyfs_dir_operations;
    root_inode->i_atime = root_inode->i_mtime = inode_set_ctime_current(root_inode);
 
    block[1].mode = mode;
    block[1].dir_children = 0;
    block[1].idx = 1;
    block[1].busy = 1;
    root_inode->i_private = &block[1];
 
    tinyfs_dbg("%s root inode %llx", __func__, root_inode);
    sb->s_root = d_make_root(root_inode);
    curr_count ++;
 
    return 0;
}
 
static struct dentry *tinyfs_mount(struct file_system_type *fs_type, int flags, const char* dev_name, void *data)
{
    return mount_nodev(fs_type, flags, data, tinyfs_fill_super);
}
 
static void tinyfs_kill_superblock(struct super_block *sb)
{
    kill_anon_super(sb);
}
 
struct file_system_type tinyfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "tinyfs",
    .mount = tinyfs_mount,
    .kill_sb = tinyfs_kill_superblock,
};
 
static int tinyfs_init(void)
{
    int ret;
 
    memset(block, 0, sizeof(block));
    ret = register_filesystem(&tinyfs_fs_type);
 
    if (ret)
        pr_info("register tinyfs filesystem failed\n");
 
    return ret;
}
 
static void tinyfs_exit(void)
{
    unregister_filesystem(&tinyfs_fs_type);
}
 
module_init(tinyfs_init);
module_exit(tinyfs_exit);
 
MODULE_LICENSE("GPL");