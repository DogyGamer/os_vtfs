#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <linux/fs.h>
#include <linux/fs_context.h>

#define MODULE_NAME "vtfs"



MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandr Plushev");
MODULE_DESCRIPTION("A amazing 67 vtfs vfs fs");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) pr_err("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

//fs ops
static int vtfs_init(struct fs_context *fs_ctx);
static int vtfs_get_tree(struct fs_context *fs_ctx);
static int vtfs_fill_super(struct super_block *sb, struct fs_context *fc);

//file ops
static int vtfs_iterate(struct file *f, struct dir_context *dir_ctx);

//inode ops
static struct dentry *vtfs_lookup(struct inode *parent, struct dentry *child, unsigned int flags);

//описание этой фс, которое передастся ядру
static struct file_system_type vtfs_type = {
    .name = "vtfs",
    .owner = THIS_MODULE,
    .init_fs_context = &vtfs_init,
    //хелпер помогающий освободить ресурсы инстанса фс
    .kill_sb = kill_anon_super
};


static struct file_operations vtfs_file_ops = {
  .iterate_shared = vtfs_iterate,
};

static struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
};

//структура с операциями над нашей фс, их дергает ядро когда ему хочца
static struct fs_context_operations vtfs_ops = {
    //при монтировании для получения дерева фс
    .get_tree = vtfs_get_tree,
};

//Вызывает vfs чтобы получить дерево фс при монтировании
static int vtfs_get_tree (struct fs_context *fs_ctx) {
    //эдакий хелпер для фс без блочных устройств
    //помогает получить superblock и заполнить его через метод вторым аргументом
    return get_tree_nodev(fs_ctx, &vtfs_fill_super);
}

//получение информации непосредственно о файле например stat
static struct dentry *vtfs_lookup(struct inode *parent, struct dentry *child, unsigned int) {

    if (strcmp((const char *)child->d_name.name, "67_gazan.txt") == 0) {
        //Почему не через child->d_sb?
        struct inode *n_inode = new_inode(parent->i_sb);
        // не понимаю тут полноценно нужно повторить то что было в vtfs_fill_super
        //Типа все опсы добавить и прочее?


        //если не получилось, вернем ошибку неудалось выделить память
        if (!n_inode) {
            return ERR_PTR(-ENOMEM);
        }

        //номер иноды, раз корень будет первый миллион я первый чемпион
        n_inode->i_ino = 2;

        //Так правильнее и заодно устанавливает владельца и группу нулл потому что нет родителя мы сделали корень
        inode_init_owner(&nop_mnt_idmap, n_inode, parent,
                     S_IFREG | 0755);


        set_nlink(n_inode, 1);

        //хелпер для временных меток
        simple_inode_init_ts(n_inode);



        d_add(child, n_inode);
        return NULL;
    }

    //если файл не найден то свяжем dentry с null
    d_add(child, NULL);
    return NULL;
}

//Используется при ls, для того чтобы посмотреть
static int vtfs_iterate(struct file *f, struct dir_context *dir_ctx) {

    LOG("vtfs_iterate %lld \n", dir_ctx->pos);

    if (!dir_emit_dots(f, dir_ctx))
        return 0;


    //фиктивный файл
    if (dir_ctx->pos == 2) {
        if (!dir_emit(dir_ctx, "67_gazan.txt", 12, 2, DT_REG))
            return 0;
        dir_ctx->pos = 3;
    }

    return 0;
}

//заполняет супреблок
static int vtfs_fill_super(struct super_block *sb, struct fs_context *fc) {
    sb->s_magic = 0x67676767;

    //сама должна выделить память
    struct inode *root_inode = new_inode(sb);

    //если не получилось, вернем ошибку неудалось выделить память
    if (!root_inode) {
        return -ENOMEM;
    }

    //номер иноды, раз корень будет первый миллион я первый чемпион
    root_inode->i_ino = 1;
    //директория с правами 755
    // root_inode-> i_mode = S_IFDIR | 0755;

    //Так правильнее и заодно устанавливает владельца и группу нулл потому что нет родителя мы сделали корень
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL,
                 S_IFDIR | 0755);

    //хелперы линукс реализующие простые базовые операции
    // root_inode->i_op = &simple_dir_inode_operations;
    // root_inode->i_fop = &simple_dir_operations;

    //уже свои операции
    root_inode->i_op = &vtfs_inode_ops;
    root_inode->i_fop = &vtfs_file_ops;

    // root_inode->i_nlink = 2;
    //. и .. поэтому 2 лол
    set_nlink(root_inode, 2);

    //хелпер для временных меток
    simple_inode_init_ts(root_inode);

    //Метод который из иноды сам сделает нам корневую dentry
    sb->s_root = d_make_root(root_inode);
    //если не получилось выделить память под dentry структуру
    if (!sb->s_root)
        return -ENOMEM;

    return 0;
}

//хранит информацию при подготовке монтирования
//контекст может хранить параметры монтирования, методы, инфу какую-то
static int vtfs_init(struct fs_context *fs_ctx) {
    fs_ctx->ops = &vtfs_ops;
    return 0;
}

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
