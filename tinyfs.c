#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
 
#include "tinyfs.h"
 
#define tinyfs_dbg pr_debug
#define tinyfs_err pr_err
 
struct file_blk block[MAX_FILES+1];
 
int curr_count = 0;
 
//获取一个可用的block 块
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
 
 // inode操作函数结构体集合
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
 
 // 文件操作函数
const struct file_operations tinyfs_file_operations = {
    // 读文件
    .read = tinyfs_read,
    // 写文件
    .write = tinyfs_write,
};
 // 目录操作函数
static const struct file_operations tinyfs_dir_operations = {
    .owner = THIS_MODULE,
    // 读取目录函数
    .read = generic_read_dir,
    // 函数用于在共享锁定模式下迭代目录内容
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

 
/**
 * @brief 创建目录
 *
 * 在给定的目录 `dir` 下，使用给定的模式 `mode` 创建一个新目录，并将其与 `dentry` 关联。
 *
 * @param idmap 挂载点的 ID 映射结构体指针
 * @param dir 父目录的 inode 结构体指针
 * @param dentry 新目录的 dentry 结构体指针
 * @param mode 目录的权限模式
 *
 * @return 成功返回 0，失败返回错误码
 */
static int tinyfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
    return tinyfs_do_create(&nop_mnt_idmap, dir, dentry, S_IFDIR | mode);
}
 
/**
 * @brief 创建文件或目录
 *
 * 在指定的目录（dir）下，根据给定的文件名（dentry）和模式（mode），使用指定的 ID 映射（idmap）创建文件或目录。
 * 如果 `excl` 为真，则当文件已存在时，创建操作将失败。
 *
 * @param idmap ID 映射结构体指针
 * @param dir 父目录的 inode 结构体指针
 * @param dentry 要创建的文件或目录的 dentry 结构体指针
 * @param mode 文件或目录的权限和类型
 * @param excl 如果为真，当文件已存在时创建失败
 *
 * @return 成功返回 0，失败返回错误码
 */
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
 // 文件系统函数，删除目录
int tinyfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    // 获取目录项对应的inode结构体
    struct inode *inode = dentry->d_inode;
    // 将inode的私有数据强制转换为file_blk结构体指针
    struct file_blk *blk = (struct file_blk*) inode->i_private;

    // 将file_blk的busy成员设置为0，表示当前块不处于忙碌状态
    blk->busy = 0;

    // 调用simple_rmdir函数来删除目录项
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
 
 // inode操作ops 函数表
static struct inode_operations tinyfs_inode_ops = {
    // 创建inode
    .create = tinyfs_create,
    // 查找功能
    .lookup = tinyfs_lookup,
    // 创建文件夹,文件夹也需要个inode
    .mkdir = tinyfs_mkdir,
    // 删除目录
    .rmdir = tinyfs_rmdir,
    // 删除文件
    .unlink = tinyfs_unlink,
};

// 创建根 root inode函数，在mount 时回调使用
int tinyfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *root_inode;
    umode_t mode = S_IFDIR;

    // 创建一个新的inode节点作为根节点
    root_inode = new_inode(sb);
    root_inode->i_ino = 1;
    // 初始化inode的所有者
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, mode);
    root_inode->i_sb = sb;
    // 设置inode的操作函数
    root_inode->i_op = &tinyfs_inode_ops;
    root_inode->i_fop = &tinyfs_dir_operations;
    // 设置inode的访问时间和修改时间为当前时间
    root_inode->i_atime = root_inode->i_mtime = inode_set_ctime_current(root_inode);

    // block[0] 作为super block
    // 初始化block[1]作为根目录的block信息
    block[1].mode = mode;
    block[1].dir_children = 0;
    block[1].idx = 1;
    block[1].busy = 1;
    // 将block[1]的地址保存到根inode的私有数据中
    root_inode->i_private = &block[1];

    // 输出调试信息
    tinyfs_dbg("%s root inode %llx", __func__, root_inode);
    // 将根inode节点作为文件系统的根目录
    sb->s_root = d_make_root(root_inode);
    // 递增当前挂载的文件系统计数
    curr_count ++;

    return 0;
}
 
 // 文件系统挂载调用函数
static struct dentry *tinyfs_mount(struct file_system_type *fs_type, int flags, const char* dev_name, void *data)
{   
    // 传入回调 创建根inode 函数
    return mount_nodev(fs_type, flags, data, tinyfs_fill_super);
}
 
static void tinyfs_kill_superblock(struct super_block *sb)
{
    kill_anon_super(sb);
}
 
 // 文件系统类型说明变量，包括文件系统名称，文件系统mount函数，删除超级快函数
 // 注册文件系统时调用 register_filesystem
struct file_system_type tinyfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "tinyfs",
    .mount = tinyfs_mount,
    .kill_sb = tinyfs_kill_superblock,
};
 
// 文件系统模块init函数
static int tinyfs_init(void)
{
    int ret;

    // 初始化块内存为0
    memset(block, 0, sizeof(block));

    // 注册文件系统
    ret = register_filesystem(&tinyfs_fs_type);

    // 如果注册失败
    if (ret)
        // 打印注册失败的信息
        pr_info("register tinyfs filesystem failed\n");

    return ret;
}
// 文件系统模块退出调用函数 
static void tinyfs_exit(void)
{
    // 注销文件系统
    unregister_filesystem(&tinyfs_fs_type);
}
 
module_init(tinyfs_init);
module_exit(tinyfs_exit);
 
MODULE_LICENSE("GPL");