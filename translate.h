#ifndef __TRANSLATE_H__
#define __TRANSLATE_H__

/**
 * enum tftp_server_instance_id_type - subsystem instance IDs
 *
 * Each value identifies a specific remote subsystem that connects to the
 * TFTP server.  The IDs match the raw instance values published via QRTR
 * (qrtr_publish) and received in QRTR_TYPE_NEW_SERVER control packets.
 */
enum tftp_server_instance_id_type {
	TFTP_SERVER_INSTANCE_ID_MSM_MPSS = 1,
	TFTP_SERVER_INSTANCE_ID_MSM_ADSP,
	TFTP_SERVER_INSTANCE_ID_MDM_MPSS,
	TFTP_SERVER_INSTANCE_ID_MDM_ADSP,
	TFTP_SERVER_INSTANCE_ID_MDM_TN,
	TFTP_SERVER_INSTANCE_ID_APQ_GSS,
	TFTP_SERVER_INSTANCE_ID_MSM_SLPI,
	TFTP_SERVER_INSTANCE_ID_MDM_SLPI,
	TFTP_SERVER_INSTANCE_ID_MSM_CDSP,
	TFTP_SERVER_INSTANCE_ID_MDM_CDSP,
	TFTP_SERVER_INSTANCE_ID_MAX,
};

int translate_open(const char *path, int flags);
int translate_folders_init(void);

#endif
