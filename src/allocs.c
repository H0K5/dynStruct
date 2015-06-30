#include "dr_api.h"
#include "dr_ir_opnd.h"
#include "drwrap.h"
#include "drmgr.h"
#include "../includes/utils.h"
#include "../includes/block_utils.h"
#include "../includes/call.h"

static int malloc_init = 0;
static int realloc_init = 0;

// maybe the following solution to get the previous pc is better
// take the ddr of the start of function on the stack (when the stack handle plt)
// use decode_next_pc to parcour the instructions and when decode_next_pc return 
// the retaddr take the pc give to decode_next_pc as addr calling

// TODO each time the addr on stack is taken, also take the ptr to the sym string

void *get_prev_instr_pc(void *pc, void *drc)
{
  instr_t	*instr = instr_create(drc);
  void		*tmp_pc;

  for (int ct = 1; ; ct++)
    {
      tmp_pc = dr_app_pc_for_decoding(pc - ct);
      if (decode(drc, tmp_pc, instr))
	{
	  if (instr_is_call(instr) && decode_next_pc(drc, tmp_pc) == pc)
	    break;
	}
      instr_reuse(drc, instr);
    }
  instr_destroy(drc, instr);
  return tmp_pc;
}

void pre_calloc(void *wrapctx, OUT void **user_data)
{
  stack_t	*stack;
  void		*drc;
  
  drc = drwrap_get_drcontext(wrapctx);

  dr_mutex_lock(lock);

  stack = drmgr_get_tls_field(drc, tls_stack_idx);
  *user_data = add_block((size_t)drwrap_get_arg(wrapctx, 1) *
			 (size_t)drwrap_get_arg(wrapctx, 0),
			 get_prev_instr_pc(drwrap_get_retaddr(wrapctx), drc),
			 stack->addr);

  dr_mutex_unlock(lock);
}

void post_calloc(void *wrapctx, void *user_data)
{
  malloc_t      *block = (malloc_t *)user_data;

  dr_mutex_lock(lock);

  if (block)
    set_addr_malloc(block, drwrap_get_retval(wrapctx), ALLOC, 0);

  dr_mutex_unlock(lock);
}

void pre_malloc(void *wrapctx, OUT void **user_data)
{
  stack_t	*stack;
  void		*drc;
  
  drc = drwrap_get_drcontext(wrapctx);

  dr_mutex_lock(lock);

  // if is the first call of malloc it's an init call and we have to do nothing
  if (!malloc_init)
    {
      malloc_init++;
      dr_mutex_unlock(lock);
      return;
    }

  stack = drmgr_get_tls_field(drc, tls_stack_idx);

  *user_data = add_block((size_t)drwrap_get_arg(wrapctx, 0),
			 get_prev_instr_pc(drwrap_get_retaddr(wrapctx), drc),
			 stack->addr);

  dr_mutex_unlock(lock);
}

void post_malloc(void *wrapctx, void *user_data)
{
  malloc_t      *block = (malloc_t *)user_data;

  dr_mutex_lock(lock);

  // first malloc don't set user_data because is call for init
  if (!user_data)
    {
      dr_mutex_unlock(lock);
      return;
    }

  if (block)
    set_addr_malloc(block, drwrap_get_retval(wrapctx), ALLOC, 0);

  dr_mutex_unlock(lock);
}

void pre_realloc(void *wrapctx, OUT void **user_data)
{
  malloc_t      *block;
  realloc_tmp_t *tmp = NULL;
  void          *start = drwrap_get_arg(wrapctx, 0);
  size_t        size = (size_t)drwrap_get_arg(wrapctx, 1);

  dr_mutex_lock(lock);

  // if is the first call of realloc it's an init call and we have to do nothing
  if (!realloc_init)
    {
      realloc_init++;
      dr_mutex_unlock(lock);
      return;
    }

  // if size == 0 => realloc call free
  if (!size)
    {
      dr_mutex_unlock(lock);
      return;
    }

  if (!(tmp = dr_global_alloc(sizeof(realloc_tmp_t))))
    {
      dr_printf("dr_malloc fail\n");
      return;
    }

  // if realloc is use like a malloc save the size to set it on the post wrapping
  tmp->size = size;
  *user_data = tmp;

  // if start == 0 => realloc call malloc
  if (!start)
    {
      tmp->block = NULL;
      dr_mutex_unlock(lock);
      return;
    }

  if (!(block = get_block_by_addr(start)))
    dr_printf("Realloc on %p error : addr was not previously malloc\n", start);
  else
    block->size = size;
  tmp->block = block;

  dr_mutex_unlock(lock);
}

void post_realloc(void *wrapctx, void *user_data)
{
  malloc_t      *block;
  void          *ret = drwrap_get_retval(wrapctx);
  stack_t       *stack;
  void		*drc;
  
  drc = drwrap_get_drcontext(wrapctx);
  dr_mutex_lock(lock);

  // if user_data is not set realloc was called to do a free or is the first call to realloc
  if (user_data)
    {
      if (((realloc_tmp_t *)user_data)->block)
        {
	  set_addr_malloc(((realloc_tmp_t *)user_data)->block, ret,
			((realloc_tmp_t *)user_data)->block->flag, 1);
	  ((realloc_tmp_t *)user_data)->block->alloc_pc =
	    get_prev_instr_pc(drwrap_get_retaddr(wrapctx), drc);
	  stack = drmgr_get_tls_field(drc, tls_stack_idx);
	  ((realloc_tmp_t *)user_data)->block->alloc_start_func_pc = stack->addr;
	}
      // if realloc is use like a malloc set the size (malloc wrapper receive a null size)
      // maybe add a linked lsit to store all pc for realloc maybe not because realloc is usualy done on array
      else if ((block = get_block_by_addr(ret)))
        block->size = ((realloc_tmp_t*)user_data)->size;
      dr_global_free(user_data, sizeof(realloc_tmp_t));
    }

  dr_mutex_unlock(lock);
}

void pre_free(void *wrapctx, __attribute__((unused))OUT void **user_data)
{
  malloc_t      *block;
  stack_t	*stack;
  void		*drc;
  
  // free(0) du nothing
  if (!drwrap_get_arg(wrapctx,0))
    return;

  drc = drwrap_get_drcontext(wrapctx);
  dr_mutex_lock(lock);
  block = get_block_by_addr(drwrap_get_arg(wrapctx, 0));
  // if the block was previously malloc we set it to free
  if (block)
    {
      stack = drmgr_get_tls_field(drc, tls_stack_idx);
      block->flag |= FREE;
      block->free_pc = get_prev_instr_pc(drwrap_get_retaddr(wrapctx), drc);
      block->free_start_func_pc = stack->addr;
    }
  else
    dr_printf("free of non alloc adress : %p\n", drwrap_get_arg(wrapctx, 0));

  dr_mutex_unlock(lock);
}
