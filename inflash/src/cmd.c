/*
 * Authors: Marek Lindner <lindner_marek@yahoo.de>
 *          Xiangfu Liu <xiangfu.z@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 3 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include "cmd.h"
#include "ingenic_cfg.h"
#include "ingenic_usb.h"
#include "usb_boot_defines.h"

extern int com_argc;
extern char com_argv[MAX_ARGC][MAX_COMMAND_LENGTH];

struct ingenic_dev ingenic_dev;
struct hand hand;

struct nand_in nand_in;
static struct nand_out nand_out;
unsigned int total_size;
unsigned char code_buf[4 * 512 * 1024];
unsigned char check_buf[4 * 512 * 1024];
unsigned char cs[16];
unsigned char ret[8];

static const char IMAGE_TYPE[][30] = {
	"with oob and ecc",
	"with oob and without ecc",
	"without oob",
};

static int load_file(struct ingenic_dev *ingenic_dev, const char *file_path)
{
	struct stat fstat;
	int fd, status, res = -1;

	status = stat(file_path, &fstat);

	if (status < 0) {
		fprintf(stderr, "Error - can't get file size from '%s': %s\n",
			file_path, strerror(errno));
		goto out;
	}

	ingenic_dev->file_len = fstat.st_size;
	ingenic_dev->file_buff = code_buf;

	fd = open(file_path, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "Error - can't open file '%s': %s\n", 
			file_path, strerror(errno));
		goto out;
	}

	status = read(fd, ingenic_dev->file_buff, ingenic_dev->file_len);

	if (status < ingenic_dev->file_len) {
		fprintf(stderr, "Error - can't read file '%s': %s\n", 
			file_path, strerror(errno));
		goto close;
	}

	/* write args to code */
	memcpy(ingenic_dev->file_buff + 8, &hand.fw_args, 
	       sizeof(struct fw_args));

	res = 1;

close:
	close(fd);
out:
	return res;
}

/* after upload stage2. must init device */
void init_cfg()
{
	if (usb_get_ingenic_cpu(&ingenic_dev) < 3) {
		printf("\n XBurst CPU not booted yet, boot it first!\n");
		return;
	}

	ingenic_dev.file_buff = &hand;
	ingenic_dev.file_len = sizeof(hand);
	if (usb_send_data_to_ingenic(&ingenic_dev) != 1)
		goto xout;

	if (usb_ingenic_configration(&ingenic_dev, DS_hand) != 1)
		goto xout;

	if (usb_read_data_from_ingenic(&ingenic_dev, ret, 8) != 1)
		goto xout;

	printf("Configuring XBurst CPU succeeded.\n");
	return;
xout:
	printf("Configuring XBurst CPU failed.\n");
}

int boot(char *stage1_path, char *stage2_path){
	int status;

	status = usb_get_ingenic_cpu(&ingenic_dev);
	switch (status)	{
	case 1:            /* Jz4740v1 */
		status = 0;
		hand.fw_args.cpu_id = 0x4740;
		break;
	case 2:            /* Jz4750v1 */
		status = 0;
		hand.fw_args.cpu_id = 0x4750;
		break;
	case 3:            /* Boot4740 */
		status = 1;
		hand.fw_args.cpu_id = 0x4740;
		break;
	case 4:            /* Boot4750 */
		status = 1;
		hand.fw_args.cpu_id = 0x4750;
		break;
	default:
		return 1;
	}

	if (status) {
		printf("Already booted.");
		return 1;
	} else {
		printf("CPU not yet booted, now booting...\n");

		/* now we upload the boot stage1 */
		printf(" Loading stage1 from '%s'\n", stage1_path);
		if (load_file(&ingenic_dev, stage1_path) < 1)
			return -1;

		if (usb_ingenic_upload(&ingenic_dev, 1) < 1)
			return -1;

		/* now we upload the boot stage2 */
		sleep(1);
		printf(" Loading stage2 from '%s'\n", stage2_path);
		if (load_file(&ingenic_dev, stage2_path) < 1)
			return -1;

		if (usb_ingenic_upload(&ingenic_dev, 2) < 1)
			return -1;

		printf("Booted successfully!\n");
	}
	sleep(1);
	init_cfg();
	return 1;
}

/* nand function  */
int error_check(unsigned char *org,unsigned char * obj,unsigned int size)
{
	unsigned int i;
	for (i = 0; i < size; i++) {
		if (org[i] != obj[i]) {
			printf("\n Check Error! %d = %x : %x ", 
			       i, org[i], obj[i]);
			return 0;
		}
	}
	return 1;
}

int nand_markbad(struct nand_in *nand_in)
{
	if (usb_get_ingenic_cpu(&ingenic_dev) < 3) {
		printf("\n Device unboot! Boot it first!");
		return -1;
	}
	printf("\n mark bad block : %d ",nand_in->start);
	usb_send_data_address_to_ingenic(&ingenic_dev, nand_in->start);
	usb_ingenic_nand_ops(&ingenic_dev, NAND_MARK_BAD);
	usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
	printf("\n Mark bad block at %d ",((ret[3] << 24) | 
					   (ret[2] << 16) | 
					   (ret[1] << 8)  | 
					   (ret[0] << 0)) / hand.nand_ppb);
	return 0;
}

int nand_program_check(struct nand_in *nand_in,
		       struct nand_out *nand_out,
		       unsigned int *start_page)
{
	unsigned int i, page_num, cur_page;
	unsigned short temp;

	if (nand_in->length > (unsigned int)MAX_TRANSFER_SIZE) {
		printf("\n Buffer size too long!");
		return -1;
	}

#ifdef CONFIG_NAND_OUT
	unsigned char status_buf[32];
	nand_out->status = status_buf;
	for (i = 0; i < nand_in->max_chip; i++)
		(nand_out->status)[i] = 0; /* set all status to fail */
#endif

	if (usb_get_ingenic_cpu(&ingenic_dev) < 3) {
		printf("\n Device unboot! Boot it first!");
		return -1;
	}
	ingenic_dev.file_buff = nand_in->buf;
	ingenic_dev.file_len = nand_in->length;
	usb_send_data_to_ingenic(&ingenic_dev);
	/* dump_data(nand_in->buf, 100); */
	for (i = 0; i < nand_in->max_chip; i++) {
		if ((nand_in->cs_map)[i]==0) 
			continue;
		if (nand_in->option == NO_OOB) {
			page_num = nand_in->length / hand.nand_ps;
			if ((nand_in->length % hand.nand_ps) !=0) 
				page_num++;
		} else {
			page_num = nand_in->length / 
				(hand.nand_ps + hand.nand_os);
			if ((nand_in->length% (hand.nand_ps + hand.nand_os)) !=0)
				page_num++;
		}
		temp = ((nand_in->option << 12) & 0xf000)  + 
			((i<<4) & 0xff0) + NAND_PROGRAM;	
		usb_send_data_address_to_ingenic(&ingenic_dev, nand_in->start);
		usb_send_data_length_to_ingenic(&ingenic_dev, page_num);
		usb_ingenic_nand_ops(&ingenic_dev, temp);

		usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
		printf(" Finish! ");

		usb_send_data_address_to_ingenic(&ingenic_dev, nand_in->start);
		/* Read back to check! */
		usb_send_data_length_to_ingenic(&ingenic_dev, page_num);

		switch (nand_in->option) {
		case OOB_ECC:
			temp = ((OOB_ECC << 12) & 0xf000) +
				((i << 4) & 0xff0) + NAND_READ;
			usb_ingenic_nand_ops(&ingenic_dev, temp);
			printf("Checking...");			
			usb_read_data_from_ingenic(&ingenic_dev, check_buf, 
						   page_num * (hand.nand_ps + hand.nand_os));
			usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
			break;
		case OOB_NO_ECC:	/* do not support data verify */
			temp = ((OOB_NO_ECC << 12) & 0xf000) + 
				((i << 4) & 0xff0) + NAND_READ;
			usb_ingenic_nand_ops(&ingenic_dev, temp);
			printf("Checking...");			
			usb_read_data_from_ingenic(&ingenic_dev, check_buf, 
						   page_num * (hand.nand_ps + hand.nand_os));
			usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
			break;
		case NO_OOB:
			temp = ((NO_OOB << 12) & 0xf000) + 
				((i << 4) & 0xff0) + NAND_READ;
			usb_ingenic_nand_ops(&ingenic_dev, temp);
			printf("Checking...");			
			usb_read_data_from_ingenic(&ingenic_dev, check_buf, 
						   page_num * hand.nand_ps);
			usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
			break;
		default:
			;
		}

		cur_page = (ret[3] << 24) | (ret[2] << 16) |  (ret[1] << 8) | 
			(ret[0] << 0);

		if (nand_in->start < 1 && 
		    hand.nand_ps == 4096 && 
		    hand.fw_args.cpu_id == 0x4740) {
			/* (nand_out->status)[i] = 1; */
			printf(" no check! End at %d ",cur_page);
			continue;
		}

		if (nand_in->check(nand_in->buf, check_buf, nand_in->length)) {
			/* (nand_out->status)[i] = 1; */
			printf(" pass! End at %d ",cur_page);
		} else {
			/* (nand_out->status)[i] = 0; */
			printf(" fail! End at %d ",cur_page);

			struct nand_in bad;
			bad.start = (cur_page - 1) / hand.nand_ppb;
			if (cur_page % hand.nand_ppb == 0)
				nand_markbad(&bad);
		}
	}

	*start_page = cur_page;
	return 0;
}

int nand_erase(struct nand_in *nand_in)
{
	unsigned int start_blk, blk_num, end_block;
	int i;

	start_blk = nand_in->start;
	blk_num = nand_in->length;
	if (start_blk > (unsigned int)NAND_MAX_BLK_NUM)  {
		printf("\n Start block number overflow!");
		return -1;
	}
	if (blk_num > (unsigned int)NAND_MAX_BLK_NUM) {
		printf("\n Length block number overflow!");
		return -1;
	}

	if (usb_get_ingenic_cpu(&ingenic_dev) < 3) {
		printf("\n Device unboot! Boot it first!");
		return -1;
	}

	for (i = 0; i < nand_in->max_chip; i++) {
		if ((nand_in->cs_map)[i]==0) 
			continue;
		printf("\n Erasing No.%d device No.%d flash......",
		       nand_in->dev, i);

		usb_send_data_address_to_ingenic(&ingenic_dev, start_blk);
		usb_send_data_length_to_ingenic(&ingenic_dev, blk_num);

		unsigned short temp = ((i << 4) & 0xff0) + NAND_ERASE;
		usb_ingenic_nand_ops(&ingenic_dev, temp);

		usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
		printf(" Finish!");
	}
	end_block = ((ret[3] << 24) |
		     (ret[2] << 16) |
		     (ret[1] << 8)  | 
		     (ret[0] << 0)) / hand.nand_ppb;
	printf("\n Operation end position : %d ",end_block);
	if (!hand.nand_force_erase) {	
	/* not force erase, show bad block infomation */
		printf("\n There are marked bad blocks :%d ", 
		       end_block - start_blk - blk_num );
	} else {
	/* force erase, no bad block infomation can show */
		printf("\n Force erase ,no bad block infomation !" );
	}
	return 1;
}

int nand_program_file(struct nand_in *nand_in,
		      struct nand_out *nand_out,
		      char *fname)
{

	int flen, m, j, k;
	unsigned int start_page = 0, page_num, code_len, offset, transfer_size;
	int fd, status;
	struct stat fstat;
	struct nand_in n_in;
	struct nand_out n_out;

#ifdef CONFIG_NAND_OUT
	unsigned char status_buf[32];
	nand_out->status = status_buf;
	for (i=0; i<nand_in->max_chip; i++)
		(nand_out->status)[i] = 0; /* set all status to fail */
#endif
	status = stat(fname, &fstat);

	if (status < 0) {
		fprintf(stderr, "Error - can't get file size from '%s': %s\n",
			fname, strerror(errno));
		return -1;
	}
	flen = fstat.st_size;

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Error - can't open file '%s': %s\n", 
			fname, strerror(errno));
		return -1;
	}

	printf("\n Programing No.%d device...",nand_in->dev);
	n_in.start = nand_in->start / hand.nand_ppb; 
	if (nand_in->option == NO_OOB) {
		if (flen % (hand.nand_ppb * hand.nand_ps) == 0) 
			n_in.length = flen / (hand.nand_ps * hand.nand_ppb);
		else
			n_in.length = flen / (hand.nand_ps * hand.nand_ppb) + 1;
	} else {
		if (flen % (hand.nand_ppb * (hand.nand_ps + hand.nand_os)) == 0) 
			n_in.length = flen / 
				((hand.nand_ps + hand.nand_os) * hand.nand_ppb);
		else
			n_in.length = flen / 
				((hand.nand_ps + hand.nand_os) * hand.nand_ppb)
				+ 1;
	}
	/* printf("\n length %d flen %d ", n_in.length, flen); */
	n_in.cs_map = nand_in->cs_map;
	n_in.dev = nand_in->dev;
	n_in.max_chip = nand_in->max_chip;
	if (nand_erase(&n_in) != 1)
		return -1;
	if (nand_in->option == NO_OOB)
		transfer_size = (hand.nand_ppb * hand.nand_ps);
	else
		transfer_size = (hand.nand_ppb * (hand.nand_ps + hand.nand_os));
	m = flen / transfer_size;
	j = flen % transfer_size;
	printf("\n Total size to send in byte is :%d", flen);
	printf("\n Image type : %s", IMAGE_TYPE[nand_in->option]);
	printf("\n It will cause %d times buffer transfer.", j == 0 ? m : m + 1);

#ifdef CONFIG_NAND_OUT
	for (i = 0; i < nand_in->max_chip; i++)
		(nand_out->status)[i] = 1; /* set all status to success! */
#endif

	offset = 0; 
	for (k = 0; k < m; k++)	{
		if (nand_in->option == NO_OOB)
			page_num = transfer_size / hand.nand_ps;
		else
			page_num = transfer_size / (hand.nand_ps + hand.nand_os);

		code_len = transfer_size;
		status = read(fd, code_buf, code_len);
		if (status < code_len) {
			fprintf(stderr, "Error - can't read file '%s': %s\n", 
				fname, strerror(errno));
			return -1;
		}

		/* read code from file to buffer */
		printf("\n No.%d Programming...",k+1);
		nand_in->length = code_len; /* code length,not page number! */
		nand_in->buf = code_buf;
		if (nand_program_check(nand_in, &n_out, &start_page) == -1)
			return -1;

		if (start_page - nand_in->start > hand.nand_ppb)
			printf("\n Skip a old bad block !");
		nand_in->start = start_page;

#ifdef CONFIG_NAND_OUT
		for (i = 0; i < nand_in->max_chip; i++) {
			(nand_out->status)[i] = (nand_out->status)[i] * 
				(n_out.status)[i];
		}
#endif
		offset += code_len ;
		//lseek(fd, offset, SEEK_SET);
	}

	if (j) {
		code_len = j;
		j += hand.nand_ps - (j % hand.nand_ps);
		memset(code_buf, 0, j);		/* set all to null */

		status = read(fd, code_buf, j);

		if (status < code_len) {
			fprintf(stderr, "Error - can't read file '%s': %s\n", 
				fname, strerror(errno));
			return -1;
		}

		nand_in->length = j;
		nand_in->buf = code_buf;
		printf("\n No.%d Programming...", k+1);

		if (nand_program_check(nand_in, &n_out, &start_page) == -1) 
			return -1;

		if (start_page - nand_in->start > hand.nand_ppb)
			printf(" Skip a old bad block !");

#ifdef CONFIG_NAND_OUT
		for (i=0; i < nand_in->max_chip; i++) {
			(nand_out->status)[i] = (nand_out->status)[i] *
				(n_out.status)[i];
		}
#endif
	}
	
	close(fd);
	return 1;
}

int nand_program_file_planes(struct nand_in *nand_in,
		      struct nand_out *nand_out,
		      char *fname)
{
	printf(" \n not implement yet !");
	return -1;
}

int init_nand_in(void)
{
	nand_in.buf = code_buf;
	nand_in.check = error_check;
	nand_in.dev = 0;
	nand_in.cs_map = cs;
	memset(nand_in.cs_map, 0, MAX_DEV_NUM);

	nand_in.max_chip = 16;
	return 0;
}

int nand_prog(void)
{
	unsigned int i;
	char *image_file;
	char *help = "\n Usage: nprog (1) (2) (3) (4) (5)"
		"\n (1)\tstart page number"
		"\n (2)\timage file name"
		"\n (3)\tdevice index number"
		"\n (4)\tflash index number"
		"\n (5) image type must be:"
		"\n \t-n:\tno oob"
		"\n \t-o:\twith oob no ecc"
		"\n \t-e:\twith oob and ecc";

	if (com_argc != 6) {
		printf("\n not enough argument.");
		printf("%s", help);
		return 0;
	}

	init_nand_in();

	nand_in.start = atoi(com_argv[1]);
	image_file = com_argv[2];
	nand_in.dev = atoi(com_argv[3]);
	(nand_in.cs_map)[atoi(com_argv[4])] = 1;
	if (!strcmp(com_argv[5], "-e")) 
		nand_in.option = OOB_ECC;
	else if (!strcmp(com_argv[5], "-o")) 
		nand_in.option = OOB_NO_ECC;
	else if (!strcmp(com_argv[5], "-n")) 
		nand_in.option = NO_OOB;
	else
		printf("%s", help);

	if (hand.nand_plane > 1)
		nand_program_file_planes(&nand_in, &nand_out, image_file);
	else
		nand_program_file(&nand_in, &nand_out, image_file);

#ifdef CONFIG_NAND_OUT
	printf("\n Flash check result:");
	for (i = 0; i < 16; i++)
		printf(" %d", (nand_out.status)[i]);
#endif

	return 1;
}

int nand_query(void)
{
	int i;
	unsigned char csn;

	if (com_argc < 3) {
		printf("\n Usage:");
		printf(" nquery (1) (2) ");
		printf("\n (1):device index number"
		       "\n (2):flash index number"); 
		return -1;
	}
	init_nand_in();

	nand_in.dev = atoi(com_argv[1]);
	(nand_in.cs_map)[atoi(com_argv[2])] = 1;

	for (i = 0; i < nand_in.max_chip; i++) {
		if ((nand_in.cs_map)[i] != 0) 
			break;
	}
	if (i >= nand_in.max_chip) 
		return -1;

	if (usb_get_ingenic_cpu(&ingenic_dev) < 3) {
		printf("\n Device unboot! Boot it first!");
		return -1;
	}

	csn = i;
	printf("\n ID of No.%d device No.%d flash: ", nand_in.dev, csn);

	unsigned short ops = ((csn << 4) & 0xff0) + NAND_QUERY;
	usb_ingenic_nand_ops(&ingenic_dev, ops);
	usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
	printf("\n Vendor ID    :0x%x ",(unsigned char)ret[0]);
	printf("\n Product ID   :0x%x ",(unsigned char)ret[1]);
	printf("\n Chip ID      :0x%x ",(unsigned char)ret[2]);
	printf("\n Page ID      :0x%x ",(unsigned char)ret[3]);
	printf("\n Plane ID     :0x%x ",(unsigned char)ret[4]);

	usb_read_data_from_ingenic(&ingenic_dev, ret, 8);
	printf("\n Operation status: Success!");

	return 1;
}
