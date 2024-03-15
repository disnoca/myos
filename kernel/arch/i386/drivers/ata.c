/**
 * Code for the ATA driver.
 * 
 * Refer to:
 * https://wiki.osdev.org/ATA_PIO_Mode
 * 
 * @author Samuel Pires
*/

#include <kernel/arch/i386/drivers/ata.h>

#include <kernel/system.h>
#include <kernel/arch/i386/io.h>

#include <stdio.h>
#include <string.h>


#define PRIMARY_PORT_BASE 		0x1F0
#define SECONDARY_PORT_BASE 	0x170

#define PRIMARY_IRQ				0x14
#define ECONDARY_IRQ			0x15

#define IDENTIFY_NUM_WORDS		256

#define SECTOR_DEFAULT_SIZE		512

#define PORT_DATA				0x0
#define PORT_ERROR				0x1
#define PORT_FEATURES			0x1
#define PORT_SECTOR_COUNT		0x2
#define PORT_LBA_LOW			0x3
#define PORT_LBA_MID			0x4
#define PORT_LBA_HI				0x5
#define PORT_DEVICE_SELECT		0x6
#define PORT_COMMAND			0x7
#define PORT_STATUS				0x7
#define PORT_CONTROL			0x206

#define COMMAND_IDENTIFY		0xEC
#define COMMAND_WRITE_SECTORS	0x30
#define COMMAND_READ_SECTORS	0x20
#define COMMAND_FLUSH			0xE7

#define STATUS_ERR				1
#define STATUS_DRQ				(1 << 3)
#define STATUS_DF				(1 << 5)	/* Drive Fault Error */
#define STATUS_BSY				(1 << 7)

#define POLL_ERR_MASK			(STATUS_ERR | STATUS_DF)


typedef struct ata_device_s {
	uint16_t port_base;
	bool master;

	uint32_t logical_sector_size;
	uint32_t physical_sector_size;
	uint16_t logical_sector_alignment;

	/* if sectors = 0, mode is not supported */
	uint32_t lba28_sectors;
	union {
		struct {
			uint32_t lba48_sectors_low;
			uint32_t lba48_sectors_high;
		};
		uint64_t lba48_sectors;
	};

	uint8_t supported_udma_modes;	/* mode n is supported if bit n-1 is set */
	uint8_t active_udma_mode;		/* active UDMA mode */
} ata_device_t;


/* An array of connected ATA devices */
/* Only the first num_ata_devices devices are valid. */
ata_device_t devs[4];

/* The ID of the currently selected device */
unsigned char selected_dev_id;


static bool ata_device_init(ata_device_t* dev, uint16_t port_base, bool master);
static bool ata_identify(ata_device_t* dev, uint16_t* buf);

static bool ata_read28(ata_device_t* dev, uint16_t* buf, uint32_t lba, uint8_t sector_count);
static bool ata_write28(ata_device_t* dev, uint16_t* data, uint32_t lba, uint8_t sector_count);
static bool ata_read48(ata_device_t* dev, uint16_t* buf, uint64_t lba, uint8_t sector_count);
static bool ata_write48(ata_device_t* dev, uint16_t* data, uint64_t lba, uint8_t sector_count);

static void ata_select(ata_device_t* dev);
static void ata_io_wait(ata_device_t* dev);
static bool ata_poll(ata_device_t* dev);


/* Global Functions */

unsigned char ata_init(void)
{
	ASSERT(num_ata_devices == 0);

	num_ata_devices += ata_device_init(devs, PRIMARY_PORT_BASE, true);
	num_ata_devices += ata_device_init(devs + num_ata_devices, PRIMARY_PORT_BASE, false);
	num_ata_devices += ata_device_init(devs + num_ata_devices, SECONDARY_PORT_BASE, true);
	num_ata_devices += ata_device_init(devs + num_ata_devices, SECONDARY_PORT_BASE, false);

	ata_select(devs);
	selected_dev_id = 0;

	return num_ata_devices;
}


void ata_write(unsigned char dev_id, void* data, uintptr_t offset, size_t size)
{
	ASSERT(dev_id < num_ata_devices);

	ata_device_t* dev = devs + dev_id;
	if(dev_id != selected_dev_id)
		ata_select(dev);

	ata_write28(dev, data, offset / dev->logical_sector_size, size / dev->logical_sector_size);
}

void ata_read(unsigned char dev_id, void* buf, uintptr_t offset, size_t size)
{
	ASSERT(dev_id < num_ata_devices);

	ata_device_t* dev = devs + dev_id;
	ata_read28(devs + dev_id, buf, offset / dev->logical_sector_size, size / dev->logical_sector_size);
}


/* Helper Functions */

/**
 * Initializes an ATA dev.
 * 
 * @param dev the device to initialize
 * @param port_base the base IO port that connects to the disk controller
 * @param master whether the disk is master or not (slave)
 * 
 * @return true if the dev is connected, false otherwise
*/
static bool ata_device_init(ata_device_t* dev, uint16_t port_base, bool master)
{
	dev->port_base = port_base;
	dev->master = master;

	uint16_t buf[IDENTIFY_NUM_WORDS];

	if(!ata_identify(dev, buf))
		return false;

	/* Words 60 & 61 taken as a uint32_t contain the total number of 28 bit LBA addressable sectors on the drive */
	dev->lba28_sectors = (buf[61] << 16) | buf[60];

	/* Word 83: Bit 10 is set if the drive supports LBA48 mode */
	if(buf[83] & (1 << 10)) {
		/*  Words 100 through 103 taken as a uint64_t contain the total number of 48 bit addressable sectors on the drive */
		dev->lba48_sectors_high = (buf[103] << 16) | buf[102];
		dev->lba48_sectors_low = (buf[101] << 16) | buf[100];
	}
	else {
		dev->lba48_sectors = 0;
	}

	/* Word 88 */
	/* The lower bytes tells which UDMA modes are supported */
	dev->supported_udma_modes = buf[88] & 0xFF;
	/* The upper byte tells which UDMA mode is active */
	dev->active_udma_mode = buf[88] >> 8;

	/* Retrieve info about drive's logical and physical sectors */
	if(!(buf[106] & (1 << 15)) && buf[106] & (1 << 14))
	{
		if(buf[106] & (1 << 12))
			dev->logical_sector_size = ((buf[118] << 16) | buf[117]) * sizeof(uint16_t);
		else
			dev->logical_sector_size = SECTOR_DEFAULT_SIZE;

		if(buf[106] & (1 << 13))
			dev->physical_sector_size = dev->logical_sector_size << (buf[106] & 0x0F);
		else
			dev->physical_sector_size = dev->logical_sector_size;
	}
	else {
		dev->logical_sector_size = SECTOR_DEFAULT_SIZE;
		dev->physical_sector_size = SECTOR_DEFAULT_SIZE;
	}

	if(buf[106] & (1 << 13) && !(buf[209] & (1 << 15)) && buf[209] & (1 << 14))
		dev->logical_sector_alignment = buf[209] & 0xC;
	else
		dev->logical_sector_alignment = 0;

	return true;
}

/**
 * Runs the ATA IDENTIFY command on a dev.
 * 
 * @param dev the device to run the command on
 * @param buf a buffer to store the data returned by the IDENTIFY command
 * 
 * @return true if the dev is connected, is ATA, and no errors ocurred; false otherwise
*/
static bool ata_identify(ata_device_t* dev, uint16_t* buf)
{
	ata_select(dev);

	/* Set the Sectorcount, LBAlo, LBAmid, and LBAhi IO ports to 0 */
	outb(dev->port_base + PORT_SECTOR_COUNT, 0);
	outb(dev->port_base + PORT_LBA_LOW, 0);
	outb(dev->port_base + PORT_LBA_MID, 0);
	outb(dev->port_base + PORT_LBA_HI, 0);
	
	/* Send the IDENTIFY command to the Command IO port */
	outb(dev->port_base + PORT_COMMAND, COMMAND_IDENTIFY);

	/* Read the Status port. If the value read is 0, the drive does not exist */
	if(!inb(dev->port_base + PORT_STATUS))
		return false;

	uint8_t status;

	/* Poll the Status port until BSY clears */
	unsigned int timer = 0xFFFFFF;
	while(--timer) {
		status = inb(dev->port_base + PORT_STATUS);

		if(!(status & STATUS_BSY))
			break;

		if(status & STATUS_ERR)
			return false;
	}

	if(timer == 0)
		return false;

	/* Check the LBAmid and LBAhi ports to see if they are non-zero. If so, the drive is not ATA */
	if(inb(dev->port_base + PORT_LBA_MID) || inb(dev->port_base + PORT_LBA_HI))
		return false;

	/* Poll the Status port until DRQ sets */
	timer = 0xFFFFFF;
	while(--timer) {
		status = inb(dev->port_base + PORT_STATUS);

		if(status & STATUS_DRQ)
			break;

		if(status & STATUS_ERR)
			return false;
	}

	if(timer == 0)
		return false;

	/* Read the data from the IDENTIFY command */
	for(int i = 0; i < IDENTIFY_NUM_WORDS; i++)
		buf[i] = inw(dev->port_base + PORT_DATA);

	return true;
}


/**
 * Reads data from an ATA device using 28-bit PIO.
 * 
 * Assumes that the device is already selected.
 * 
 * @param dev the device
 * @param buf the buffer to read the data to
 * @param lba the LBA
 * @param sector_count the number of sectors to read
 * 
 * @return true if the read completed successfully, false otherwise
*/
static bool ata_read28(ata_device_t* dev, uint16_t* buf, uint32_t lba, uint8_t sector_count)
{
	/* Assert that the lba is addressable with 28 bits */
	ASSERT(!(lba & 0xF0000000));

	/* Send 0xE0 for the "master" or 0xF0 for the "slave", ORed with the highest 4 bits of the LBA to the drive select port */
	outb(dev->port_base + PORT_DEVICE_SELECT, (dev->master ? 0xE0 : 0xF0) | (lba >> 24));

	/* Send the sectorcount to the sectorcount port */
	outb(dev->port_base + PORT_SECTOR_COUNT, sector_count);

	/* Send the LBA to the LBA ports */
	outb(dev->port_base + PORT_LBA_LOW, lba);
	outb(dev->port_base + PORT_LBA_MID, lba >> 8);
	outb(dev->port_base + PORT_LBA_HI, lba >> 16);

	/* Send the "READ SECTORS" command to the command port */
	outb(dev->port_base + PORT_COMMAND, COMMAND_READ_SECTORS);

	for(int i = 0; i < sector_count; i++)
	{
		/* Wait until device is ready */
		if(!ata_poll(dev))
			return false;

		/* Transfer 256 16-bit values, a word at a time, from the data port into your buffer */
		for(int i = 0; i < dev->logical_sector_size / sizeof(uint16_t); i++)
			buf[i] = inw(dev->port_base + PORT_DATA);

		/* After transferring the last word of a PIO data block to the data IO port, give the drive a 400ns delay
		   to reset its DRQ bit (and possibly set BSY again, while emptying/filling its buffer to/from the drive) */
		for(int i = 0; i < 400; i++)
			if(!(inb(dev->port_base + PORT_DATA) & STATUS_DRQ))
				break;
	}

	return true;
}


/**
 * Writes data to an ATA device using 28-bit PIO.
 * 
 * Assumes that the device is already selected.
 * 
 * @param dev the device
 * @param data an array to write the data from
 * @param lba the LBA
 * @param sector_count the number of sectors to write
 * 
 * @return true if the write completed successfully, false otherwise
*/
static bool ata_write28(ata_device_t* dev, uint16_t* data, uint32_t lba, uint8_t sector_count)
{
	/* Assert that the lba is addressable with 28 bits */
	ASSERT(!(lba & 0xF0000000));

	/* Send 0xE0 for the "master" or 0xF0 for the "slave", ORed with the highest 4 bits of the LBA to the drive select port */
	outb(dev->port_base + PORT_DEVICE_SELECT, (dev->master ? 0xE0 : 0xF0) | (lba >> 24));

	/* Send the sectorcount to the sectorcount port */
	outb(dev->port_base + PORT_SECTOR_COUNT, sector_count);

	/* Send the LBA to the LBA ports */
	outb(dev->port_base + PORT_LBA_LOW, lba);
	outb(dev->port_base + PORT_LBA_MID, lba >> 8);
	outb(dev->port_base + PORT_LBA_HI, lba >> 16);

	/* Send the "WRITE SECTORS" command (0x30) to the command port */
	outb(dev->port_base + PORT_COMMAND, COMMAND_WRITE_SECTORS);

	for(int i = 0; i < sector_count; i++)
	{
		/* Wait until device is ready*/
		if(!ata_poll(dev))
			return false;

		/* Transfer 256 16-bit values, a word at a time, from memory into the data port */
		for(int i = 0; i < dev->logical_sector_size / sizeof(uint16_t); i++)
			outw(dev->port_base + PORT_DATA, data[i]);

		/* Flush the data */
		outb(dev->port_base + PORT_COMMAND, COMMAND_FLUSH);
		while(inb(dev->port_base + PORT_STATUS) & STATUS_BSY) {}
	}

	return true;
}


static bool ata_read48(ata_device_t* dev, uint16_t* buf, uint64_t lba, uint8_t sector_count)
{

}

static bool ata_write48(ata_device_t* dev, uint16_t* buf, uint64_t lba, uint8_t sector_count)
{

}


/**
 * Selects a drive to be used.
 * 
 * @param dev the device to be selected
*/
static void ata_select(ata_device_t* dev)
{
	/* Select a target drive by sending 0xA0 for the master drive, or 0xB0 for the slave, to the "drive select" IO port */
	outb(dev->port_base + PORT_DEVICE_SELECT, dev->master ? 0xA0 : 0xB0);
	ata_io_wait(dev);
}

/**
 * Polls an ATA device until it becomes ready.
 * 
 * Assumes that the device is already selected.
 * 
 * @param dev the device
 * 
 * @return true if the device is ready, false if an error ocurred
*/
static bool ata_poll(ata_device_t* dev)
{
	/* Read the Regular Status port until the BSY bit clears, and the DRQ bit sets or until the ERR bit or DF bit sets.
	   If neither error bit is set, the device is ready. */
	int timer = 0xFFFFFF;
	while(--timer) {
		uint8_t status = inb(dev->port_base + PORT_STATUS);

		if(!(status & STATUS_BSY) && (status & STATUS_DRQ))
			break;

		if(status & POLL_ERR_MASK)
			return false;
	}

	return timer > 0;
}

static void ata_io_wait(ata_device_t* dev)
{
	/* Wait 400 nanoseconds */
	inb(dev->port_base + PORT_CONTROL);
	inb(dev->port_base + PORT_CONTROL);
	inb(dev->port_base + PORT_CONTROL);
	inb(dev->port_base + PORT_CONTROL);
}