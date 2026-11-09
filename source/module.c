#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <linux/fs.h>
#include <linux/fs_context.h>


#include <linux/slab.h>
#include <linux/err.h>
#include <linux/dcache.h>

#include <linux/uaccess.h>
#include "vfs/vtfs.h"
#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandr Plushev");
MODULE_DESCRIPTION("A amazing 67 vtfs vfs fs");

//описание этой фс, которое передастся ядру
struct file_system_type vtfs_type = {
    .name = "vtfs",
    .owner = THIS_MODULE,
    .init_fs_context = &vtfs_init_fs_context,
    //хелпер помогающий освободить ресурсы инстанса фс
    .kill_sb = kill_anon_super
};

//insmod hook
static int __init vtfs_module_init(void) {
    int ret;
    LOG("VTFS joined the kernel\n");

    //сообщить ядру о новой фс
    ret = register_filesystem(&vtfs_type);

    if (ret != 0) {
        ERR("VTFS registration failed: %d\n", ret);
        return ret;
    }

    LOG("VTFS registered\n");
    return 0;
}

//rmmod hook
static void __exit vtfs_exit(void) {
    LOG("VTFS left the kernel\n");
    unregister_filesystem(&vtfs_type);
}

module_init(vtfs_module_init);
module_exit(vtfs_exit);