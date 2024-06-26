#include "linux/list.h"
#include "linux/mm_types.h"
#include "linux/printk.h"
#include "linux/sched.h"
#include "linux/vmalloc.h"
#include <asm-generic/errno-base.h>
#include <asm/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/rmap.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/debugfs.h>

#define DEBUGFS_DIR_NAME "at"
#define DEBUGFS_FILE_NAME "at"

static struct dentry *debugfs_dir;
static struct dentry *debugfs_file;
void add_string_to_list(const char *fmt, ...);
void __exit address_translation_exit(void);
int __init address_translation_init(void);
extern struct list_head vmap_area_list;

struct task_data {
	struct task_struct *task;
	struct vm_area_struct *vma;
} task_data;

struct rwc_args {
	unsigned long long addr_to_be_resolved;
};

struct my_node {
	struct list_head list;
	char *data;
};

LIST_HEAD(read_data);

// Function to add a formatted string to the linked list
void add_string_to_list(const char *fmt, ...)
{
	va_list args;
	struct my_node *new_node;
	int len;

	// Calculate the length of the formatted string
	va_start(args, fmt);
	len = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	if (len < 0) {
		printk(KERN_ERR "Failed to get string length\n");
		return;
	}

	new_node = kmalloc(sizeof(struct my_node), GFP_KERNEL);
	if (!new_node) {
		printk(KERN_ERR "Failed to allocate memory for new node\n");
		return;
	}

	new_node->data = kmalloc(len + 1, GFP_KERNEL);
	if (!new_node->data) {
		printk(KERN_ERR "Failed to allocate memory for data\n");
		kfree(new_node);
		return;
	}

	// Format the string
	va_start(args, fmt);
	vsnprintf(new_node->data, len + 1, fmt, args);
	va_end(args);
	INIT_LIST_HEAD(&new_node->list);
	list_add_tail(&new_node->list, &read_data);
}

/*
 * Code copied from Linux Kernel Source
 * Idle page tracking only considers user memory pages, for other types of
 * pages the idle flag is always unset and an attempt to set it is silently
 * ignored.
 *
 * We treat a page as a user memory page if it is on an LRU list, because it is
 * always safe to pass such a page to rmap_walk(), which is essential for idle
 * page tracking. With such an indicator of user pages we can skip isolated
 * pages, but since there are not usually many of them, it will hardly affect
 * the overall result.
 *
 * This function tries to get a user memory page by pfn as described above.
 */
static struct folio *get_folio(unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);
	struct folio *folio;
	if (!page || PageTail(page))
		return NULL;
	folio = page_folio(page);
	if (!folio_test_lru(folio) || !folio_try_get(folio))
		return NULL;
	if (unlikely(page_folio(page) != folio || !folio_test_lru(folio))) {
		folio_put(folio);
		folio = NULL;
	}
	return folio;
}

static unsigned long long get_physical_address(unsigned long virt_addr,
					       struct task_struct *task)
{
	struct mm_struct *task_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	struct page *page = NULL;
	unsigned long offset_within_page;
	unsigned long long phys_addr = 0;

	task_mm = task->mm;
	// acquire page table lock
	spin_lock(&(task_mm->page_table_lock));

	pgd = pgd_offset(task_mm, virt_addr);
	if (pgd_none(*pgd))
		printk(KERN_EMERG "No pgd");

	p4d = p4d_offset(pgd, virt_addr);
	if (p4d_none(*p4d))
		printk(KERN_EMERG "No p4d");

	pud = pud_offset(p4d, virt_addr);
	if (pud_none(*pud))
		printk(KERN_EMERG "No pud");

	pmd = pmd_offset(pud, virt_addr);
	if (pmd_none(*pmd))
		printk(KERN_EMERG "No pmd");

	pte = pte_offset_kernel(pmd, virt_addr);
	if (pte_present(*pte)) {
		page = pte_page(*pte);
		offset_within_page = (virt_addr) & (PAGE_SIZE - 1);
		phys_addr = page_to_phys(page) + offset_within_page;
	} else
		printk(KERN_EMERG "No pte present");
	pte_unmap(pte);
	// Release spin lock
	spin_unlock(&(task_mm->page_table_lock));
	return phys_addr;
}

static int at_open(struct inode *inode, struct file *filp)
{
	pr_info("Address translation Device Open\n");
	return 0; /* success */
}

static int at_release(struct inode *inode, struct file *filp)
{
	pr_info("Address translation Device Close\n");
	return 0;
}

static bool folio_data(struct folio *folio, struct vm_area_struct *vma,
		       unsigned long address, void *arg)
{
	struct task_struct *task = vma->vm_mm->owner;
	struct rwc_args data = *(struct rwc_args *)arg;
	unsigned long page_start = address;
	unsigned long long phys_addr_page_start;
	unsigned int offset_within_page;
	phys_addr_page_start = get_physical_address(page_start, task);
	offset_within_page = data.addr_to_be_resolved - phys_addr_page_start;
	add_string_to_list("Virtual Address of 0x%llx is 0x%lx with pid %d\n",
			   data.addr_to_be_resolved,
			   address + offset_within_page, task->pid);
	return true;
}

static bool analyse_vmalloc_memory(const unsigned long long addr)
{
	struct vmap_area *va;
	bool found = false;
	void *start, *end;
	int nr_pages;
	/*
	 * This means that the memory doesn't belongs to User Space
	 * Let's check for kernel space before actually throwing an error
	 */
	list_for_each_entry(va, &vmap_area_list, list) {
		nr_pages = va->vm->nr_pages;
		if (nr_pages <= 0)
			continue;
		start = page_address(va->vm->pages[0]);
		end = page_address(va->vm->pages[va->vm->nr_pages - 1]) +
		      PAGE_SIZE;
		if (addr >= (unsigned long long)start &&
		    addr < (unsigned long long)end) {
			found = true;
			break;
		}
	}
	if (!found) {
		pr_err("Physical address not mapped to KVA: 0x%llx\n", addr);
		return false;
	}
	add_string_to_list("VA start: 0x%lx\n", va->va_start);
	return true;
}

static void analyse_physical_address(const unsigned long long addr)
{
	struct rwc_args data;
	struct folio *folio;
	if (analyse_vmalloc_memory(addr))
		return;
	folio = get_folio(PHYS_PFN(addr));
	data.addr_to_be_resolved = addr;
	struct rmap_walk_control rwc = {
		.rmap_one = folio_data,
		.arg = (void *)&data,
	};

	if (folio != NULL) {
		rmap_walk(folio, &rwc);
		return;
	}
	pr_err("Physical address 0x%llx is not mapped to any user process\n",
	       addr);
	return;
}

static ssize_t at_write(struct file *filp, const char __user *buf, size_t count,
			loff_t *f_pos)
{
	char *data = (char *)kmalloc(4096, GFP_KERNEL);
	unsigned long long addr;
	memset(data, 0, 4096);
	if (copy_from_user(data, buf, count))
		return -EFAULT;
	if (kstrtoull(data, 16, &addr)) {
		pr_warn("invalid address '%s'\n", data);
		return -EFAULT;
	}
	pr_info("Device Wrote %lu bytes: 0x%llX", count, addr);
	analyse_physical_address(addr);
	return count;
}

static ssize_t at_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	struct my_node *cur;
	ssize_t bytes_to_copy;

	if (list_empty(&read_data)) {
		return 0;
	}

	cur = list_first_entry(&read_data, struct my_node, list);

	if (!cur)
		return 0; // No more data to read

	bytes_to_copy = strlen(cur->data);

	if (copy_to_user(buf, cur->data, strlen(cur->data))) {
		return -EFAULT;
	}

	// Delete the node from the list and free memory
	list_del(&cur->list);
	kfree(cur->data);
	kfree(cur);

	*ppos += bytes_to_copy;

	return bytes_to_copy;
}

static struct file_operations at_fops = {
	.owner = THIS_MODULE,
	.read = at_read,
	.write = at_write,
	.open = at_open,
	.release = at_release,
};

void __exit address_translation_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);
	pr_info("Address Translation Module Unloaded\n");
}

int __init address_translation_init(void)
{
	debugfs_dir = debugfs_create_dir(DEBUGFS_DIR_NAME, NULL);
	if (!debugfs_dir) {
		pr_err("Failed to create debugfs directory\n");
		return -ENOMEM;
	}

	debugfs_file = debugfs_create_file(DEBUGFS_FILE_NAME, 0644, debugfs_dir,
					   NULL, &at_fops);
	if (!debugfs_file) {
		pr_err("Failed to create debugfs file\n");
		debugfs_remove_recursive(debugfs_dir);
		return -ENOMEM;
	}
	return 0;
}

module_init(address_translation_init);
module_exit(address_translation_exit);

MODULE_AUTHOR("Mukesh Kumar Chaurasiya");
MODULE_DESCRIPTION("Address Translation Module");
MODULE_LICENSE("GPL");
