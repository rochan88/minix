/* The kernel call implemented in this file:
 *   m_type:	SYS_PRIVCTL
 *
 * The parameters for this kernel call are:
 *    m2_i1:	CTL_ENDPT 	(process endpoint of target)
 *    m2_i2:	CTL_REQUEST	(privilege control request)
 *    m2_p1:	CTL_ARG_PTR	(pointer to request data)
 */

#include "../system.h"
#include "../ipc.h"
#include <signal.h>
#include <string.h>

#if USE_PRIVCTL

/*===========================================================================*
 *				do_privctl				     *
 *===========================================================================*/
PUBLIC int do_privctl(m_ptr)
message *m_ptr;			/* pointer to request message */
{
/* Handle sys_privctl(). Update a process' privileges. If the process is not
 * yet a system process, make sure it gets its own privilege structure.
 */
  register struct proc *caller_ptr;
  register struct proc *rp;
  int proc_nr;
  int priv_id;
  int ipc_to_m, kcalls;
  int i, r;
  struct io_range io_range;
  struct mem_range mem_range;
  struct priv priv;
  int irq;

  /* Check whether caller is allowed to make this call. Privileged proceses 
   * can only update the privileges of processes that are inhibited from 
   * running by the RTS_NO_PRIV flag. This flag is set when a privileged process
   * forks. 
   */
  caller_ptr = proc_addr(who_p);
  if (! (priv(caller_ptr)->s_flags & SYS_PROC)) return(EPERM); 
  if(m_ptr->CTL_ENDPT == SELF) proc_nr = who_p;
  else if(!isokendpt(m_ptr->CTL_ENDPT, &proc_nr)) return(EINVAL);
  rp = proc_addr(proc_nr);

  switch(m_ptr->CTL_REQUEST)
  {
  case SYS_PRIV_ALLOW:
	/* Allow process to run. Make sure its privilege structure has already
	 * been set.
	 */
	if (!RTS_ISSET(rp, RTS_NO_PRIV) || priv(rp)->s_proc_nr == NONE) {
		return(EPERM);
	}
	RTS_LOCK_UNSET(rp, RTS_NO_PRIV);
	return(OK);

  case SYS_PRIV_DISALLOW:
	/* Disallow process from running. */
	if (RTS_ISSET(rp, RTS_NO_PRIV)) return(EPERM);
	RTS_LOCK_SET(rp, RTS_NO_PRIV);
	return(OK);

  case SYS_PRIV_SET_SYS:
	/* Set a privilege structure of a blocked system process. */
	if (! RTS_ISSET(rp, RTS_NO_PRIV)) return(EPERM);

	/* Check whether a static or dynamic privilege id must be allocated. */
	priv_id = NULL_PRIV_ID;
	if (m_ptr->CTL_ARG_PTR)
	{
		/* Copy privilege structure from caller */
		if((r=data_copy(who_e, (vir_bytes) m_ptr->CTL_ARG_PTR,
			SYSTEM, (vir_bytes) &priv, sizeof(priv))) != OK)
			return r;

		/* See if the caller wants to assign a static privilege id. */
		if(!(priv.s_flags & DYN_PRIV_ID)) {
			priv_id = priv.s_id;
		}
	}

	/* Make sure this process has its own privileges structure. This may
	 * fail, since there are only a limited number of system processes.
	 * Then copy privileges from the caller and restore some defaults.
	 */
	if ((i=get_priv(rp, priv_id)) != OK)
	{
		kprintf("do_privctl: unable to allocate priv_id %d: %d\n",
			priv_id, i);
		return(i);
	}
	priv_id = priv(rp)->s_id;		/* backup privilege id */
	*priv(rp) = *priv(caller_ptr);		/* copy from caller */
	priv(rp)->s_id = priv_id;		/* restore privilege id */
	priv(rp)->s_proc_nr = proc_nr;		/* reassociate process nr */

	for (i=0; i< BITMAP_CHUNKS(NR_SYS_PROCS); i++)	/* remove pending: */
	      priv(rp)->s_notify_pending.chunk[i] = 0;	/* - notifications */
	priv(rp)->s_int_pending = 0;			/* - interrupts */
	sigemptyset(&priv(rp)->s_sig_pending);		/* - signals */

	/* Set defaults for privilege bitmaps. */
	priv(rp)->s_flags= DEF_SYS_F;           /* privilege flags */
	priv(rp)->s_trap_mask= DEF_SYS_T;       /* allowed traps */
	ipc_to_m = DEF_SYS_M;                   /* allowed targets */
	kcalls = DEF_SYS_KC;                    /* allowed kernel calls */
	for(i = 0; i < CALL_MASK_SIZE; i++) {
		priv(rp)->s_k_call_mask[i] = (kcalls == NO_C ? 0 : (~0));
	}

	/* Set defaults for resources: no I/O resources, no memory resources,
	 * no IRQs, no grant table
	 */
	priv(rp)->s_nr_io_range= 0;
	priv(rp)->s_nr_mem_range= 0;
	priv(rp)->s_nr_irq= 0;
	priv(rp)->s_grant_table= 0;
	priv(rp)->s_grant_entries= 0;

	/* Override defaults if the caller has supplied a privilege structure. */
	if (m_ptr->CTL_ARG_PTR)
	{
		/* Copy s_flags. */
		priv(rp)->s_flags = priv.s_flags;

		/* Copy IRQs */
		if(priv.s_flags & CHECK_IRQ) {
			if (priv.s_nr_irq < 0 || priv.s_nr_irq > NR_IRQ)
				return EINVAL;
			priv(rp)->s_nr_irq= priv.s_nr_irq;
			for (i= 0; i<priv.s_nr_irq; i++)
			{
				priv(rp)->s_irq_tab[i]= priv.s_irq_tab[i];
#if 0
				kprintf("do_privctl: adding IRQ %d for %d\n",
					priv(rp)->s_irq_tab[i], rp->p_endpoint);
#endif
			}
		}

		/* Copy I/O ranges */
		if(priv.s_flags & CHECK_IO_PORT) {
			if (priv.s_nr_io_range < 0 || priv.s_nr_io_range > NR_IO_RANGE)
				return EINVAL;
			priv(rp)->s_nr_io_range= priv.s_nr_io_range;
			for (i= 0; i<priv.s_nr_io_range; i++)
			{
				priv(rp)->s_io_tab[i]= priv.s_io_tab[i];
#if 0
				kprintf("do_privctl: adding I/O range [%x..%x] for %d\n",
					priv(rp)->s_io_tab[i].ior_base,
					priv(rp)->s_io_tab[i].ior_limit,
					rp->p_endpoint);
#endif
			}
		}

		/* Copy memory ranges */
		if(priv.s_flags & CHECK_MEM) {
			if (priv.s_nr_mem_range < 0 || priv.s_nr_mem_range > NR_MEM_RANGE)
				return EINVAL;
			priv(rp)->s_nr_mem_range= priv.s_nr_mem_range;
			for (i= 0; i<priv.s_nr_mem_range; i++)
			{
				priv(rp)->s_mem_tab[i]= priv.s_mem_tab[i];
#if 0
				kprintf("do_privctl: adding mem range [%x..%x] for %d\n",
					priv(rp)->s_mem_tab[i].mr_base,
					priv(rp)->s_mem_tab[i].mr_limit,
					rp->p_endpoint);
#endif
			}
		}

		/* Copy trap mask. */
		priv(rp)->s_trap_mask = priv.s_trap_mask;

		/* Copy target mask. */
		memcpy(&ipc_to_m, &priv.s_ipc_to, sizeof(ipc_to_m));

		/* Copy kernel call mask. */
		memcpy(priv(rp)->s_k_call_mask, priv.s_k_call_mask,
			sizeof(priv(rp)->s_k_call_mask));
	}

	/* Fill in target mask. */
	for (i=0; i < NR_SYS_PROCS; i++) {
		if (ipc_to_m & (1 << i))
			set_sendto_bit(rp, i);
		else
			unset_sendto_bit(rp, i);
	}

	return(OK);

  case SYS_PRIV_SET_USER:
	/* Set a privilege structure of a blocked user process. */
	if (!RTS_ISSET(rp, RTS_NO_PRIV)) return(EPERM);

	/* Link the process to the privilege structure of the root user
	 * process all the user processes share.
	 */
	priv(rp) = priv_addr(USER_PRIV_ID);

	return(OK);

  case SYS_PRIV_ADD_IO:
	if (RTS_ISSET(rp, RTS_NO_PRIV))
		return(EPERM);

	/* Only system processes get I/O resources? */
	if (!(priv(rp)->s_flags & SYS_PROC))
		return EPERM;

#if 0 /* XXX -- do we need a call for this? */
	if (strcmp(rp->p_name, "fxp") == 0 ||
		strcmp(rp->p_name, "rtl8139") == 0)
	{
		kprintf("setting ipc_stats_target to %d\n", rp->p_endpoint);
		ipc_stats_target= rp->p_endpoint;
	}
#endif

	/* Get the I/O range */
	data_copy(who_e, (vir_bytes) m_ptr->CTL_ARG_PTR,
		SYSTEM, (vir_bytes) &io_range, sizeof(io_range));
	priv(rp)->s_flags |= CHECK_IO_PORT;	/* Check I/O accesses */
	i= priv(rp)->s_nr_io_range;
	if (i >= NR_IO_RANGE)
		return ENOMEM;

	priv(rp)->s_io_tab[i].ior_base= io_range.ior_base;
	priv(rp)->s_io_tab[i].ior_limit= io_range.ior_limit;
	priv(rp)->s_nr_io_range++;

	return OK;

  case SYS_PRIV_ADD_MEM:
	if (RTS_ISSET(rp, RTS_NO_PRIV))
		return(EPERM);

	/* Only system processes get memory resources? */
	if (!(priv(rp)->s_flags & SYS_PROC))
		return EPERM;

	/* Get the memory range */
	if((r=data_copy(who_e, (vir_bytes) m_ptr->CTL_ARG_PTR,
		SYSTEM, (vir_bytes) &mem_range, sizeof(mem_range))) != OK)
		return r;
	priv(rp)->s_flags |= CHECK_MEM;	/* Check memory mappings */
	i= priv(rp)->s_nr_mem_range;
	if (i >= NR_MEM_RANGE)
		return ENOMEM;

	priv(rp)->s_mem_tab[i].mr_base= mem_range.mr_base;
	priv(rp)->s_mem_tab[i].mr_limit= mem_range.mr_limit;
	priv(rp)->s_nr_mem_range++;

	return OK;

  case SYS_PRIV_ADD_IRQ:
	if (RTS_ISSET(rp, RTS_NO_PRIV))
		return(EPERM);

	/* Only system processes get IRQs? */
	if (!(priv(rp)->s_flags & SYS_PROC))
		return EPERM;

	data_copy(who_e, (vir_bytes) m_ptr->CTL_ARG_PTR,
		SYSTEM, (vir_bytes) &irq, sizeof(irq));
	priv(rp)->s_flags |= CHECK_IRQ;	/* Check IRQs */

	i= priv(rp)->s_nr_irq;
	if (i >= NR_IRQ)
		return ENOMEM;
	priv(rp)->s_irq_tab[i]= irq;
	priv(rp)->s_nr_irq++;

	return OK;
  case SYS_PRIV_QUERY_MEM:
  {
	phys_bytes addr, limit;
  	struct priv *sp;
	/* See if a certain process is allowed to map in certain physical
	 * memory.
	 */
	addr = (phys_bytes) m_ptr->CTL_PHYSSTART;
	limit = addr + (phys_bytes) m_ptr->CTL_PHYSLEN - 1;
	if(limit < addr)
		return EPERM;
	if(!(sp = priv(rp)))
		return EPERM;
	if (!(sp->s_flags & SYS_PROC))
		return EPERM;
	for(i = 0; i < sp->s_nr_mem_range; i++) {
		if(addr >= sp->s_mem_tab[i].mr_base &&
		   limit <= sp->s_mem_tab[i].mr_limit)
			return OK;
	}
	return EPERM;
  }
  default:
	kprintf("do_privctl: bad request %d\n", m_ptr->CTL_REQUEST);
	return EINVAL;
  }
}

#endif /* USE_PRIVCTL */

