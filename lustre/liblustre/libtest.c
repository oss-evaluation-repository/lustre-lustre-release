#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <portals/api-support.h> /* needed for ptpctl.h */
#include <portals/ptlctl.h>	/* needed for parse_dump */


#include <liblustre.h>
#include <linux/obd.h>
#include <linux/obd_class.h>
#include <portals/procbridge.h>

struct ldlm_namespace;
struct ldlm_res_id;
struct obd_import;

extern int ldlm_cli_cancel_unused(struct ldlm_namespace *ns, struct ldlm_res_id *res_id, int flags);
extern int ldlm_namespace_cleanup(struct ldlm_namespace *ns, int local_only);
extern int ldlm_replay_locks(struct obd_import *imp);

void *inter_module_get(char *arg)
{
        if (!strcmp(arg, "tcpnal_ni"))
                return &tcpnal_ni;
        else if (!strcmp(arg, "ldlm_cli_cancel_unused"))
                return ldlm_cli_cancel_unused;
        else if (!strcmp(arg, "ldlm_namespace_cleanup"))
                return ldlm_namespace_cleanup;
        else if (!strcmp(arg, "ldlm_replay_locks"))
                return ldlm_replay_locks;
        else
                return NULL;
}

ptl_handle_ni_t         tcpnal_ni;

struct pingcli_args {
        ptl_nid_t mynid;
        ptl_nid_t nid;
	ptl_pid_t port;
        int count;
        int size;
};

struct task_struct *current;

struct obd_class_user_state ocus;

/* portals interfaces */
ptl_handle_ni_t *
kportal_get_ni (int nal)
{
        return &tcpnal_ni;
}

inline void
kportal_put_ni (int nal)
{
        return;
}

void init_current(int argc, char **argv)
{ 
        current = malloc(sizeof(*current));
        strncpy(current->comm, argv[0], sizeof(current->comm));
        current->pid = getpid();

}

ptl_nid_t tcpnal_mynid;

int init_lib_portals(struct pingcli_args *args)
{
        int rc;

        PtlInit();
        tcpnal_mynid = args->mynid;
        rc = PtlNIInit(procbridge_interface, 0, 0, 0, &tcpnal_ni);
        if (rc != 0) {
                CERROR("ksocknal: PtlNIInit failed: error %d\n", rc);
                PtlFini();
                RETURN (rc);
        }
        PtlNIDebug(tcpnal_ni, ~0);
        return rc;
}

extern int class_handle_ioctl(struct obd_class_user_state *ocus, unsigned int cmd, unsigned long arg);


int lib_ioctl(int dev_id, int opc, void * ptr)
{

	if (dev_id == OBD_DEV_ID) {
                struct obd_ioctl_data *ioc = ptr;
		class_handle_ioctl(&ocus, opc, (unsigned long)ptr);

		/* you _may_ need to call obd_ioctl_unpack or some
		   other verification function if you want to use ioc
		   directly here */
		printf ("processing ioctl cmd: %x buf len: %d\n", 
			opc,  ioc->ioc_len);
	}
	return (0);
}

int main(int argc, char **argv) 
{
        struct pingcli_args *args;
	args= malloc(sizeof(*args));
        if (!args) { 
                printf("Malloc error\n");
                exit(1);
        }

	args->mynid   = ntohl (inet_addr (argv[1]));
        INIT_LIST_HEAD(&ocus.ocus_conns);

        init_current(argc, argv);
        init_obdclass();
        init_lib_portals(args);
        ptlrpc_init();
        ldlm_init();
        mdc_init();
        lov_init();
        osc_init();
        echo_client_init();

	parse_dump("/tmp/DUMP_FILE", lib_ioctl);

        printf("Hello\n");
        return 0;
}

