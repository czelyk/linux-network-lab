#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xb7d5d32a, "nf_register_net_hook" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xf2fc6d02, "nf_unregister_net_hook" },
	{ 0xd272d446, "__fentry__" },
	{ 0xc562ce63, "in_aton" },
	{ 0x42b8b324, "init_net" },
	{ 0xd954c786, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xb7d5d32a,
	0xe8213e80,
	0xd272d446,
	0x90a48d82,
	0xf2fc6d02,
	0xd272d446,
	0xc562ce63,
	0x42b8b324,
	0xd954c786,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"nf_register_net_hook\0"
	"_printk\0"
	"__x86_return_thunk\0"
	"__ubsan_handle_out_of_bounds\0"
	"nf_unregister_net_hook\0"
	"__fentry__\0"
	"in_aton\0"
	"init_net\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "214C511010321FD630E1384");
