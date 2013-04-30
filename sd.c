/* File:    sd.c
 * 
 * Author:  David Zemon
 */

// Includes
#include <sd.h>

/*** Global variable declarations ***/
// Initialization variables
static uint32 g_sd_cs;											// Chip select pin mask
static uint8 g_sd_filesystem;			// Filesystem type - one of SD_FAT_16 or SD_FAT_32
static uint8 g_sd_sectorsPerCluster_shift;// Used as a quick multiply/divide; Stores log_2(Sectors per Cluster)
static uint32 g_sd_rootDirSectors;			// Number of sectors for the root directory
static uint32 g_sd_fatStart;						// Starting block address of the FAT
static uint32 g_sd_rootAddr;			// Starting block address of the root directory
static uint32 g_sd_rootAllocUnit;// Allocation unit of root directory/first data sector (FAT32 only)
static uint32 g_sd_firstDataAddr;	// Starting block address of the first data cluster

// FAT filesystem variables
static uint8 g_sd_fat[SD_SECTOR_SIZE];						// Buffer for FAT entries only
#ifdef SD_FILE_WRITE
static uint8 g_sd_fatMod = 0;		// Has the currently loaded FAT sector been modified
static uint32 g_sd_fatSize;
#endif
static uint16 g_sd_entriesPerFatSector_Shift;// How many FAT entries are in a single sector of the FAT
static uint32 g_sd_curFatSector;	// Store the current FAT sector loaded into g_sd_fat

static uint32 g_sd_dir_firstAllocUnit; // Store the current directory's starting allocation unit

sd_buffer g_sd_buf;

// Assigned to a file and then to each buffer that it touches - overwritten by other functions
// and used as a check by the file to determine if the buffer needs to be reloaded with its
// sector
static uint8 g_sd_fileID = 0;

// First byte response receives special treatment to allow for proper debugging
static uint8 g_sd_firstByteResponse;

#ifdef SD_DEBUG
// variable is needed to help determine what is causing seemingly random timeouts
uint32 g_sd_sectorRdAddress;
#endif

/***********************************
 *** Public Function Definitions ***
 ***********************************/
uint8 SDStart (const uint32 mosi, const uint32 miso, const uint32 sclk, const uint32 cs) {
	uint8 i, j, k, err;
	uint8 response[16];

	// Set CS for output and initialize high
	g_sd_cs = cs;
	GPIODirModeSet(cs, GPIO_DIR_OUT);
	GPIOPinSet(cs);

	// Start SPI module
	if (err = SPIStart(mosi, miso, sclk, SD_SPI_INIT_FREQ, SD_SPI_POLARITY))
		SDError(err);

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Starting SD card...\n");
#endif

	for (i = 0; i < 10; ++i) {
		// Initialization loop (reset SD card)
		for (j = 0; j < 10; ++j) {
			GPIOPinSet(cs);
			waitcnt(CLKFREQ/10 + CNT);

			// Send at least 72 clock cycles to enable the SD card
			for (k = 0; k < 5; ++k)
				SPIShiftOut(16, -1, SD_SPI_MODE_OUT);
			GPIOPinClear(cs);

			// Send SD into idle state, retrieve a response and ensure it is the "idle" response
			if (err = SDSendCommand(SD_CMD_IDLE, 0, SD_CRC_IDLE))
				SDError(err);
			SDGetResponse(SD_RESPONSE_LEN_R1, response);
			if (SD_RESPONSE_IDLE == g_sd_firstByteResponse)
				j = 10;
#if (defined SD_VERBOSE && defined SD_DEBUG)
			else
				printf("Failed attempt at CMD0: 0x%02X\n", g_sd_firstByteResponse);
#endif
		}
		if (SD_RESPONSE_IDLE != g_sd_firstByteResponse)
			SDError(SD_INVALID_INIT);

#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("SD card in idle state. Now sending CMD8...\n");
#endif

		// Set voltage to 3.3V and ensure response is R7
		if (err = SDSendCommand(SD_CMD_SDHC, SD_CMD_VOLT_ARG, SD_CRC_SDHC))
			SDError(err);
		if (err = SDGetResponse(SD_RESPONSE_LEN_R7, response))
			SDError(err);
		if ((SD_RESPONSE_IDLE == g_sd_firstByteResponse) && (0x01 == response[2])
				&& (0xAA == response[3]))
			i = 10;
#if (defined SD_VERBOSE && defined SD_DEBUG)
		else
			printf("Failed attempt at CMD8\n");
#endif
	}

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("CMD8 succeeded. Requesting operating conditions...\n");
#endif

// Request operating conditions register and ensure response begins with R1
	if (err = SDSendCommand(SD_CMD_READ_OCR, 0, SD_CRC_OTHER))
		SDError(err);
	if (err = SDGetResponse(SD_RESPONSE_LEN_R3, response))
		SDError(err);
#if (defined SD_VERBOSE && defined SD_DEBUG)
	SDPrintHexBlock(response, SD_RESPONSE_LEN_R3);
#endif
	if (SD_RESPONSE_IDLE != g_sd_firstByteResponse)
		SDError(SD_INVALID_INIT);

// Spin up the card and bring to active state
#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("OCR read successfully. Sending into active state...\n");
#endif
	for (i = 0; i < 8; ++i) {
		waitcnt(CLKFREQ/10 + CNT);
		if (err = SDSendCommand(SD_CMD_APP, 0, SD_CRC_OTHER))
			SDError(err);
		if (err = SDGetResponse(1, response))
			SDError(err);
		if (err = SDSendCommand(SD_CMD_WR_OP, BIT_30, SD_CRC_OTHER))
			SDError(err);
		SDGetResponse(1, response);
		if (SD_RESPONSE_ACTIVE == g_sd_firstByteResponse)
			break;
#if (defined SD_VERBOSE && defined SD_DEBUG)
		else
			printf("Failed attempt at active state: 0x%02X\n", g_sd_firstByteResponse);
#endif
	}
	if (SD_RESPONSE_ACTIVE != g_sd_firstByteResponse)
		SDError(SD_INVALID_RESPONSE);
#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Activated!\n");
#endif

// Initialization nearly complete, increase clock
	SPISetClock(SD_SPI_FINAL_FREQ);

// If debugging requested, print to the screen CSD and CID registers from SD card
#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Requesting CSD...\n");
	if (err = SDSendCommand(SD_CMD_RD_CSD, 0, SD_CRC_OTHER))
		SDError(err);
	if (err = SDReadBlock(16, response))
		SDError(err);
	printf("CSD Contents:\n");
	SDPrintHexBlock(response, 16);
	putchar('\n');

	printf("Requesting CID...\n");
	if (err = SDSendCommand(SD_CMD_RD_CID, 0, SD_CRC_OTHER))
		SDError(err);
	if (err = SDReadBlock(16, response))
		SDError(err);
	printf("CID Contents:\n");
	SDPrintHexBlock(response, 16);
	putchar('\n');
#endif
	GPIOPinSet(cs);

// Initialization complete
	return 0;
}

uint8 SDMount (void) {
	uint8 err, temp;

	// FAT system determination variables:
	uint32 rsvdSectorCount, numFATs, rootEntryCount, totalSectors, FATSize, dataSectors;
	uint32 bootSector = 0;
	uint32 clusterCount;

	// Read in first sector
	if (err = SDReadDataBlock(bootSector, g_sd_buf.buf))
		SDError(err);
	// Check if sector 0 is boot sector or MBR; if MBR, skip to boot sector at first partition
	if (SD_BOOT_SECTOR_ID != g_sd_buf.buf[SD_BOOT_SECTOR_ID_ADDR]) {
		bootSector = SDReadDat32(&(g_sd_buf.buf[SD_BOOT_SECTOR_BACKUP]));
		if (err = SDReadDataBlock(bootSector, g_sd_buf.buf))
			SDError(err);
	}

	// Print the boot sector if requested
#if (defined SD_VERBOSE && defined SD_DEBUG && defined SD_VERBOSE_BLOCKS)
	printf("***BOOT SECTOR***\n");
	SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
	putchar('\n');
#endif

	// Do this whether it is FAT16 or FAT32
	temp = g_sd_buf.buf[SD_CLUSTER_SIZE_ADDR];
#if (defined SD_DEBUG && defined SD_VERBOSE)
	printf("Preliminary sectors per cluster: %u\n", temp);
#endif
	while (temp) {
		temp >>= 1;
		++g_sd_sectorsPerCluster_shift;
	}
	--g_sd_sectorsPerCluster_shift;
	rsvdSectorCount = SDReadDat16(&((g_sd_buf.buf)[SD_RSVD_SCTR_CNT_ADDR]));
	numFATs = g_sd_buf.buf[SD_NUM_FATS_ADDR];
#ifdef SD_FILE_WRITE
	if (2 != numFATs)
		SDError(SD_TOO_MANY_FATS);
#endif
	rootEntryCount = SDReadDat16(&(g_sd_buf.buf[SD_ROOT_ENTRY_CNT_ADDR]));

	// Check if FAT size is valid in 16- or 32-bit location
	FATSize = SDReadDat16(&(g_sd_buf.buf[SD_FAT_SIZE_16_ADDR]));
	if (!FATSize)
		FATSize = SDReadDat32(&(g_sd_buf.buf[SD_FAT_SIZE_32_ADDR]));

	// Check if FAT16 total sectors is valid
	totalSectors = SDReadDat16(&(g_sd_buf.buf[SD_TOT_SCTR_16_ADDR]));
	if (!totalSectors)
		totalSectors = SDReadDat32(&(g_sd_buf.buf[SD_TOT_SCTR_32_ADDR]));

	// Compute necessary numbers to determine FAT type (12/16/32)
	g_sd_rootDirSectors = (rootEntryCount * 32) >> SD_SECTOR_SIZE_SHIFT;
	dataSectors = totalSectors - (rsvdSectorCount + numFATs * FATSize + rootEntryCount);
	clusterCount = dataSectors >> g_sd_sectorsPerCluster_shift;

#if (defined SD_DEBUG && defined SD_VERBOSE)
	printf("Sectors per cluster: %u\n", 1 << g_sd_sectorsPerCluster_shift);
	printf("Reserved sector count: 0x%08X / %u\n", rsvdSectorCount, rsvdSectorCount);
	printf("Number of FATs: 0x%02X / %u\n", numFATs, numFATs);
	printf("Total sector count: 0x%08X / %u\n", totalSectors, totalSectors);
	printf("Total cluster count: 0x%08X / %u\n", clusterCount, clusterCount);
	printf("Total data sectors: 0x%08X / %u\n", dataSectors, dataSectors);
	printf("FAT Size: 0x%04X / %u\n", FATSize, FATSize);
	printf("Root directory sectors: 0x%08X / %u\n", g_sd_rootDirSectors,
			g_sd_rootDirSectors);
	printf("Root entry count: 0x%08X / %u\n", rootEntryCount, rootEntryCount);
#endif

	// Determine and store FAT type
	if (SD_FAT12_CLSTR_CNT > clusterCount)
		SDError(SD_INVALID_FILESYSTEM);
	else if (SD_FAT16_CLSTR_CNT > clusterCount) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("\n***FAT type is FAT16***\n");
#endif
		g_sd_filesystem = SD_FAT_16;
		g_sd_entriesPerFatSector_Shift = 8;
	} else {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("\n***FAT type is FAT32***\n");
#endif
		g_sd_filesystem = SD_FAT_32;
		g_sd_entriesPerFatSector_Shift = 7;
	}

	// Find start of FAT
	g_sd_fatStart = bootSector + rsvdSectorCount;

	//	g_sd_filesystem = SD_FAT_16;
	// Find root directory address
	switch (g_sd_filesystem) {
		case SD_FAT_16:
			g_sd_rootAddr = FATSize * numFATs + g_sd_fatStart;
			g_sd_firstDataAddr = g_sd_rootAddr + g_sd_rootDirSectors;
			break;
		case SD_FAT_32:
			g_sd_firstDataAddr = g_sd_rootAddr = bootSector + rsvdSectorCount
					+ FATSize * numFATs;
			g_sd_rootAllocUnit = SDReadDat32(&(g_sd_buf.buf[SD_ROOT_CLUSTER_ADDR]));
			break;
	}

#ifdef SD_FILE_WRITE
	// If files will be written to, the second FAT must also be updated - the first sector
	// address of which is stored here
	g_sd_fatSize = FATSize;
#endif

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Start of FAT: 0x%08X\n", g_sd_fatStart);
	printf("Root directory alloc. unit: 0x%08X\n", g_sd_rootAllocUnit);
	printf("Root directory sector: 0x%08X\n", g_sd_rootAddr);
	printf("Calculated root directory sector: 0x%08X\n",
			SDGetSectorFromAlloc(g_sd_rootAllocUnit));
	printf("First data sector: 0x%08X\n", g_sd_firstDataAddr);
#endif

	// Store the first sector of the FAT
	if (err = SDReadDataBlock(g_sd_fatStart, g_sd_fat))
		SDError(err);
	g_sd_curFatSector = 0;

	// Print FAT if desired
#if (defined SD_VERBOSE && defined SD_DEBUG && defined SD_VERBOSE_BLOCKS)
	printf("\n***First File Allocation Table***\n");
	SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
	putchar('\n');
#endif

	// Read in the root directory, set root as current
	if (err = SDReadDataBlock(g_sd_rootAddr, g_sd_buf.buf))
		SDError(err);
	g_sd_buf.curClusterStartAddr = g_sd_rootAddr;
	if (SD_FAT_16 == g_sd_filesystem) {
		g_sd_dir_firstAllocUnit = -1;
		g_sd_buf.curAllocUnit = -1;
	} else {
		g_sd_buf.curAllocUnit = g_sd_dir_firstAllocUnit = g_sd_rootAllocUnit;
		if (err = SDGetFATValue(g_sd_buf.curAllocUnit, &g_sd_buf.nextAllocUnit))
			return err;
	}
	g_sd_buf.curClusterStartAddr = g_sd_rootAddr;
	g_sd_buf.curSectorOffset = 0;

	// Print root directory
#if (defined SD_VERBOSE && defined SD_DEBUG && defined SD_VERBOSE_BLOCKS)
	printf("***Root directory***\n");
	SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
	putchar('\n');
#endif

	return 0;
}

uint8 SDchdir (const char *d) {
	uint8 err;
	uint16 fileEntryOffset = 0;

	g_sd_buf.id = SD_FOLDER_ID;

	// Attempt to find the file
	if (err = SDFind(d, &fileEntryOffset)) {
		return err;
	} else {
		// File entry was found successfully, load it into the buffer and update status variables
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("%s found at offset 0x%04X from address 0x%08X\n", d, fileEntryOffset,
				g_sd_buf.curClusterStartAddr + g_sd_buf.curSectorOffset);
#endif

		if (SD_FAT_16 == g_sd_filesystem)
			g_sd_buf.curAllocUnit = SDReadDat16(
					&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_LOW]));
		else {
			g_sd_buf.curAllocUnit = SDReadDat16(
					&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_LOW]));
			g_sd_buf.curAllocUnit |= SDReadDat16(
					&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_HIGH])) << 16;
			g_sd_buf.curAllocUnit &= 0x0FFFFFFF; // Clear the highest 4 bits - they are always reserved
		}
		SDGetFATValue(g_sd_buf.curAllocUnit, &(g_sd_buf.nextAllocUnit));
		if (0 == g_sd_buf.curAllocUnit) {
			g_sd_buf.curAllocUnit = -1;
			g_sd_dir_firstAllocUnit = g_sd_rootAllocUnit;
		} else
			g_sd_dir_firstAllocUnit = g_sd_buf.curAllocUnit;
		g_sd_buf.curSectorOffset = 0;

#ifdef SD_FILE_WRITE
		if (g_sd_buf.mod)
			SDWriteDataBlock(g_sd_buf.curClusterStartAddr + g_sd_buf.curSectorOffset,
					g_sd_buf.buf);
#endif
		SDReadDataBlock(g_sd_buf.curClusterStartAddr, g_sd_buf.buf);

#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Opening directory from...\n");
		printf("\tAllocation unit 0x%08X\n", g_sd_buf.curAllocUnit);
		printf("\tCluster starting address 0x%08X\n", g_sd_buf.curClusterStartAddr);
		printf("\tSector offset 0x%04X\n", g_sd_buf.curSectorOffset);
#ifdef SD_VERBOSE_BLOCKS
		printf("And the first directory sector looks like....\n");
		SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
		putchar('\n');
#endif
#endif
		return 0;
	}
}

uint8 SDfopen (const char *name, sd_file *f, const sd_file_mode mode) {
	uint8 err;
	uint16 fileEntryOffset = 0;

#if (defined SD_DEBUG && defined SD_VERBOSE)
	printf("Attempting to open %s\n", name);
#endif

	f->id = g_sd_fileID++;
	f->rPtr = 0;
	f->wPtr = 0;
#if (defined SD_DEBUG && !(defined SD_FILE_WRITE))
	if (SD_FILE_MODE_R != mode)
	SDError(SD_INVALID_FILE_MODE);
#endif
	f->mode = mode;

	// Attempt to find the file
	if (err = SDFind(name, &fileEntryOffset)) {
#ifdef SD_FILE_WRITE
		// Find returned an error, ensure it was either file-not-found or EOC and then
		// create the file
		if ((SD_EOC_END == err) && SD_FILE_MODE_R != f->mode) {
			// File wasn't found and the cluster is full; add another to the directory
#if (defined SD_VERBOSE && defined SD_DEBUG)
			printf("Directory cluster was full, adding another...\n");
#endif
			SDExtendFAT(&g_sd_buf);
			SDLoadNextSector(&g_sd_buf);
		}
		if (SD_EOC_END == err || SD_FILENAME_NOT_FOUND == err) {
			// File wasn't found, but there is still room in this cluster (or a new cluster
			// was just added)
#if (defined SD_VERBOSE && defined SD_DEBUG)
			printf("Creating a new directory entry...\n");
#endif
			SDCreateFile(name, &fileEntryOffset);
		} else
#endif
			// SDFind returned unknown error - throw it
			SDError(err);
	}

	// name was found successfully, determine if it is a file or directory
	if (SD_SUB_DIR & g_sd_buf.buf[fileEntryOffset + SD_FILE_ATTRIBUTE_OFFSET])
		SDError(SD_ENTRY_NOT_FILE);

	//Passed the file-not-directory test, load it into the buffer and update status variables
	f->buf->id = f->id;
	f->curSector = 0;
	if (SD_FAT_16 == g_sd_filesystem)
		f->buf->curAllocUnit = SDReadDat16(
				&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_LOW]));
	else {
		f->buf->curAllocUnit = SDReadDat16(
				&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_LOW]));
		f->buf->curAllocUnit |= SDReadDat16(
				&(g_sd_buf.buf[fileEntryOffset + SD_FILE_START_CLSTR_HIGH])) << 16;

		f->buf->curAllocUnit &= 0x0FFFFFFF; // Clear the highest 4 bits - they are always reserved
	}
	f->firstAllocUnit = f->buf->curAllocUnit;
	f->curCluster = 0;
	if (err = SDGetFATValue(f->buf->curAllocUnit, &(f->buf->nextAllocUnit)))
		SDError(err);
	f->buf->curClusterStartAddr = SDGetSectorFromAlloc(f->buf->curAllocUnit);
	f->buf->curSectorOffset = 0;
	f->length = SDReadDat32(&(g_sd_buf.buf[fileEntryOffset + SD_FILE_LEN_OFFSET]));
	if (err = SDReadDataBlock(f->buf->curClusterStartAddr, f->buf->buf))
		SDError(err);

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Opening file from...\n");
	printf("\tAllocation unit 0x%08X\n", f->buf->curAllocUnit);
	printf("\tNext allocation unit 0x%08X\n", f->buf->nextAllocUnit);
	printf("\tCluster starting address 0x%08X\n", f->buf->curClusterStartAddr);
	printf("\tSector offset 0x%04X\n", f->buf->curSectorOffset);
#ifdef SD_VERBOSE_BLOCKS
	printf("And the first file sector looks like....\n");
	SDPrintHexBlock(f->buf->buf, SD_SECTOR_SIZE);
	putchar('\n');
#endif
#endif

	return 0;
}

uint8 SDfclose (sd_file *f) {
	uint8 err;

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Closing file...\n");
#endif
	if (NULL != f->buf) {
#ifdef SD_FILE_WRITE
		if (f->buf->id != f->id)
			if (err = SDReloadBuf(f))
				return err;
		// If the currently loaded sector has been modified, save the changes
		if (f->buf->mod && f->buf->curClusterStartAddr != f->curCluster)
			SDWriteDataBlock(f->buf->curClusterStartAddr + f->buf->curSectorOffset,
					f->buf->buf);
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Modified sector in file has been saved...\n");
#endif
#endif
		// Set the buffer pointer to NULL, signifying a closed file
		f->buf = NULL;
	}

	return 0;
}

#ifdef SD_FILE_WRITE
uint8 SDfputc (const char c, sd_file *f) {
	uint8 err;
	uint32 numSectors;
	uint8 sectorsPerCluster = 1 << g_sd_sectorsPerCluster_shift; // Sectors per cluster is used often
	uint16 sectorPtr = f->wPtr % SD_SECTOR_SIZE; // Determines byte-offset within a sector
	uint32 sectorOffset = (f->wPtr >> SD_SECTOR_SIZE_SHIFT); // Determine the needed file sector

	// Determine if the correct sector is loaded
	if (f->buf->id != f->id)
		SDReloadBuf(f);

	// Even the the buffer was just reloaded, this snippet needs to be called in order to
	// extend the FAT if needed
	if (sectorOffset != f->curSector) {
		// Incorrect sector loaded
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Loading new file sector at file-offset: 0x%08X / %u\n", sectorOffset,
				sectorOffset);
#endif

		// If the sectorPtr points to a new sector and the sectorOffset points to a new
		// cluster, it is possible that a new cluster needs to be allocated...
		if (!sectorPtr && !(sectorOffset % sectorsPerCluster)) {
			// Find the total number of available sectors
			numSectors = f->length >> SD_SECTOR_SIZE_SHIFT;
			numSectors += sectorsPerCluster - (numSectors % sectorsPerCluster);
#if (defined SD_VERBOSE && defined SD_DEBUG)
			printf("Available sectors in the file: 0x%08X / %u\n", numSectors,
					numSectors);
#endif
			// Determine if we need to add another cluster
			if (sectorOffset > numSectors) {
				// A new cluster is needed - add it and then load it
#if (defined SD_VERBOSE && defined SD_DEBUG)
				printf("Extending the FAT!\n");
#endif
				if (err = SDExtendFAT(f->buf))
					return err;
			}
		}
		// SDLoadSectorFromOffset() will ensure that, if the current buffer has been
		// modified, it is written back to the SD card before loading a new one
		SDLoadSectorFromOffset(f, sectorOffset);
	}

	if (++(f->wPtr) >= f->length)
		++(f->length);
	f->buf->buf[sectorPtr] = c;
	f->buf->mod = 1;

	return 0;
}

uint8 SDfputs (char *s, sd_file *f) {
	uint8 err;

	while (*s)
		if (err = SDfputc(*(s++), f))
			return err;

	return 0;
}
#endif

char SDfgetc (sd_file *f) {
	char c;
	uint16 ptr = f->rPtr % SD_SECTOR_SIZE;

	// Determine if the currently loaded sector is what we need
	uint32 sectorOffset = (f->rPtr >> SD_SECTOR_SIZE_SHIFT);

	// Determine if the correct sector is loaded
	if (f->buf->id != f->id)
		SDReloadBuf(f);
	else if (sectorOffset != f->curSector) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("File sector offset: 0x%08X / %u\n", sectorOffset, sectorOffset);
#endif
		SDLoadSectorFromOffset(f, sectorOffset);
	}
	++(f->rPtr);
	c = f->buf->buf[ptr];
	return c;
}

char * SDfgets (char s[], uint32 size, sd_file *f) {
	/* Code taken from fgets.c in the propgcc source, originally written by Eric R. Smith
	 * and (slightly) modified to fit this SD driver
	 */

	uint32 c;
	uint32 count = 0;

	--size;
	while (count < size) {
		c = SDfgetc(f);
		if ((uint32) SD_EOF == c)
			break;
		s[count++] = c;
		if ('\n' == c)
			break;
	}
	s[count] = 0;
	return (0 < count) ? s : NULL;
}

inline uint8 SDfeof (sd_file *f) {
	return f->length == f->rPtr;
}

uint8 SDfseekr (sd_file *f, const int32 offset, const uint8 origin) {
	switch (origin) {
		case SEEK_SET:
			f->rPtr = offset;
			break;
		case SEEK_CUR:
			f->rPtr += offset;
			break;
		case SEEK_END:
			f->rPtr = f->length + offset - 1;
			break;
		default:
			return SD_INVALID_PTR_ORIGIN;
			break;
	}

	return 0;
}

uint8 SDfseekw (sd_file *f, const int32 offset, const uint8 origin) {
	switch (origin) {
		case SEEK_SET:
			f->wPtr = offset;
			break;
		case SEEK_CUR:
			f->wPtr += offset;
			break;
		case SEEK_END:
			f->wPtr = f->length + offset - 1;
			break;
		default:
			return SD_INVALID_PTR_ORIGIN;
			break;
	}

	return 0;
}

uint32 SDftellr (const sd_file *f) {
	return f->rPtr;
}

uint32 SDftellw (const sd_file *f) {
	return f->wPtr;
}

#ifdef SD_SHELL
uint8 SD_Shell (sd_file *f) {
	char usrInput[SD_SHELL_INPUT_LEN] = "";
	char cmd[SD_SHELL_CMD_LEN] = "";
	char arg[SD_SHELL_ARG_LEN] = "";
	char uppercaseName[SD_SHELL_ARG_LEN] = "";
	uint8 i, j, err;

	printf("Welcome to David's quick shell! There is no help, nor much to do.\n");
	printf("Have fun...\n");

// Loop until the user types the SD_SHELL_EXIT string
	while (strcmp(usrInput, SD_SHELL_EXIT)) {
		printf(">>> ");
		gets(usrInput);

#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Received \"%s\" as the complete line\n", usrInput);
#endif

		// Retrieve command
		for (i = 0; (0 != usrInput[i] && ' ' != usrInput[i]); ++i)
			cmd[i] = usrInput[i];

#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Received \"%s\" as command\n", cmd);
#endif

		// Retrieve argument if it exists (skip over spaces)
		if (0 != usrInput[i]) {
			j = 0;
			while (' ' == usrInput[i])
				++i;
			for (; (0 != usrInput[i] && ' ' != usrInput[i]); ++i)
				arg[j++] = usrInput[i];
#if (defined SD_VERBOSE && defined SD_DEBUG)
			printf("And \"%s\" as the argument\n", arg);
#endif
		}

		// Convert the arg to uppercase
		for (i = 0; arg[i]; ++i)
			if ('a' <= arg[i] && 'z' >= arg[i])
				uppercaseName[i] = arg[i] + 'A' - 'a';
			else
				uppercaseName[i] = arg[i];

		// Interpret the command
		if (!strcmp(cmd, SD_SHELL_LS))
			err = SD_Shell_ls();
		else if (!strcmp(cmd, SD_SHELL_CAT))
			err = SD_Shell_cat(uppercaseName, f);
		else if (!strcmp(cmd, SD_SHELL_CD))
			err = SDchdir(uppercaseName);
#ifdef SD_FILE_WRITE
		else if (!strcmp(cmd, SD_SHELL_TOUCH))
			err = SD_Shell_touch(uppercaseName);
#endif
#ifdef SD_VERBOSE_BLOCKS
		else if (!strcmp(cmd, "d"))
		SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
#endif
		else if (!strcmp(cmd, SD_SHELL_EXIT))
			break;
		else if (strcmp(usrInput, ""))
			printf("Invalid command: %s\n", cmd);

		// Check for errors
		if ((uint8) SD_EOC_END == err)
			printf("\tError, entry not found: \"%s\"\n", arg);
		else if ((uint8) SD_ENTRY_NOT_FILE == err)
			printf("\tError, entry not a file: \"%s\"\n", arg);
		else if ((uint8) SD_FILE_ALREADY_EXISTS == err)
			printf("\tError, file already exists: \"%s\"\n", arg);
		else if (err)
			printf("Error occurred: 0x%02X / %u\n", err, err);

		// Erase the command and argument strings
		for (i = 0; i < SD_SHELL_CMD_LEN; ++i)
			cmd[i] = 0;
		for (i = 0; i < SD_SHELL_ARG_LEN; ++i)
			uppercaseName[i] = arg[i] = 0;
		err = 0;
	}

	return 0;
}

uint8 SD_Shell_ls (void) {
	uint8 err;
	uint16 rootIdx = 0, fileEntryOffset = 0;
	char string[SD_FILENAME_STR_LEN];		// Allocate space for a filename string

// If we aren't looking at the beginning of a cluster, we must backtrack to the beginning and then begin listing files
	if (g_sd_buf.curSectorOffset
			|| (SDGetSectorFromAlloc(g_sd_dir_firstAllocUnit)
					!= g_sd_buf.curClusterStartAddr)) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("'ls' requires a backtrack to beginning of directory's cluster\n");
#endif
		g_sd_buf.curClusterStartAddr = SDGetSectorFromAlloc(g_sd_dir_firstAllocUnit);
		g_sd_buf.curSectorOffset = 0;
		g_sd_buf.curAllocUnit = g_sd_dir_firstAllocUnit;
		if (err = SDGetFATValue(g_sd_buf.curAllocUnit, &(g_sd_buf.nextAllocUnit)))
			return err;
		if (err = SDReadDataBlock(g_sd_buf.curClusterStartAddr, g_sd_buf.buf))
			return err;
	}

// Loop through all files in the current directory until we find the correct one
// Function will exit normally without an error code if the file is not found
	while (g_sd_buf.buf[fileEntryOffset]) {
		// Check if file is valid, retrieve the name if it is
		if ((SD_DELETED_FILE_MARK != g_sd_buf.buf[fileEntryOffset])
				&& !(SD_SYSTEM_FILE
						& g_sd_buf.buf[fileEntryOffset + SD_FILE_ATTRIBUTE_OFFSET]))
			SDPrintFileEntry(&(g_sd_buf.buf[fileEntryOffset]), string);

		// Increment to the next file
		fileEntryOffset += SD_FILE_ENTRY_LENGTH;

		// If it was the last entry in this sector, proceed to the next one
		if (SD_SECTOR_SIZE == fileEntryOffset) {
			// Last entry in the sector, attempt to load a new sector
			// Possible error value includes end-of-chain marker
			if (err = SDLoadNextSector(&g_sd_buf)) {
				if ((uint8) SD_EOC_END == err)
					break;
				else
					SDError(err);
			}

			fileEntryOffset = 0;
		}
	}

	return 0;
}

uint8 SD_Shell_cat (const char *name, sd_file *f) {
	uint8 err;
	char str[128];

// Attempt to find the file
	if (err = SDfopen(name, f, SD_FILE_MODE_R)) {
		if ((uint8) SD_EOC_END == err)
			return err;
		else
			SDError(err);
	} else {
		// Loop over each character and print them to the screen one-by-one
		while (!SDfeof(f)) {
			// Using SDfgetc() instead of SDfgets to ensure compatibility with binary files
//			printf("%02X\n", SDfgetc(f));
//			SDfgetc(f);
			putchar(SDfgetc(f));
		}
		putchar('\n');
	}

	return 0;
}

#ifdef SD_FILE_WRITE
uint8 SD_Shell_touch (const char name[]) {
	uint8 err;
	uint16 fileEntryOffset;

	// Attempt to find the file if it already exists
	if (err = SDFind(name, &fileEntryOffset)) {
		// Error occured - hopefully it was a "file not found" error
		if (SD_FILENAME_NOT_FOUND == err)
			// File wasn't found, let's create it
			err = SDCreateFile(name, &fileEntryOffset);
		return err;
	}

	// If SDFind() returns 0, the file already existed and an error should be thrown
	return SD_FILE_ALREADY_EXISTS;
}
#endif
#endif

#if (defined SD_VERBOSE || defined SD_VERBOSE_BLOCKS)
uint8 SDPrintHexBlock (uint8 *dat, uint16 bytes) {
	uint8 i, j;
	uint8 *s;

	printf("Printing %u bytes...\n", bytes);
	printf("Offset\t");
	for (i = 0; i < SD_LINE_SIZE; ++i)
		printf("0x%X  ", i);
	putchar('\n');

	if (bytes % SD_LINE_SIZE)
		bytes = bytes / SD_LINE_SIZE + 1;
	else
		bytes /= SD_LINE_SIZE;

	for (i = 0; i < bytes; ++i) {
		s = (uint8 *) (dat + SD_LINE_SIZE * i);
		printf("0x%04X:\t", SD_LINE_SIZE * i);
		for (j = 0; j < SD_LINE_SIZE; ++j)
			printf("0x%02X ", s[j]);
		printf(" - ");
		for (j = 0; j < SD_LINE_SIZE; ++j) {
			if ((' ' <= s[j]) && (s[j] <= '~'))
				putchar(s[j]);
			else
				putchar('.');
		}

		putchar('\n');
	}

	return 0;
}
#endif

/************************************
 *** Private Function Definitions ***
 ************************************/
uint8 SDSendCommand (const uint8 cmd, const uint32 arg, const uint8 crc) {
	uint8 err;

// Send out the command
	if (err = SPIShiftOut(8, cmd, SD_SPI_MODE_OUT))
		return err;

// Send argument
	if (err = SPIShiftOut(16, (arg >> 16), SD_SPI_MODE_OUT))
		return err;
	if (err = SPIShiftOut(16, arg & WORD_0, SD_SPI_MODE_OUT))
		return err;

// Send sixth byte - CRC
	if (err = SPIShiftOut(8, crc, SD_SPI_MODE_OUT))
		return err;

	return 0;
}

uint8 SDGetResponse (uint8 bytes, uint8 *dat) {
	uint8 err;
	uint32 timeout;

// Read first byte - the R1 response
	timeout = SD_RESPONSE_TIMEOUT + CNT;
	do {
		if (err = SPIShiftIn(8, SD_SPI_MODE_IN, &g_sd_firstByteResponse,
				SD_SPI_BYTE_IN_SZ))
			return err;

		// Check for timeout
		if (0 < (timeout - CNT) && (timeout - CNT) < SD_WIGGLE_ROOM)
			return SD_READ_TIMEOUT;
	} while (0xff == g_sd_firstByteResponse); // wait for transmission end

	if ((SD_RESPONSE_IDLE == g_sd_firstByteResponse)
			|| (SD_RESPONSE_ACTIVE == g_sd_firstByteResponse)) {
		--bytes;	// Decrement bytes counter

		// Read remaining bytes
		while (bytes--)
			if (err = SPIShiftIn(8, SD_SPI_MODE_IN, dat++, SD_SPI_BYTE_IN_SZ))
				return err;
	} else
		return SD_INVALID_RESPONSE;

	if (err = SPIShiftOut(8, 0xff, SD_SPI_MODE_OUT))
		return err;

	return 0;
}

uint8 SDReadBlock (uint16 bytes, uint8 *dat) {
	uint8 i, err, checksum;
	uint32 timeout;

// Read first byte - the R1 response
	timeout = SD_RESPONSE_TIMEOUT + CNT;
	do {
		if (err = SPIShiftIn(8, SD_SPI_MODE_IN, &g_sd_firstByteResponse,
				SD_SPI_BYTE_IN_SZ))
			return err;

		// Check for timeout
		if (0 < (timeout - CNT) && (timeout - CNT) < SD_WIGGLE_ROOM)
			return SD_READ_TIMEOUT;
	} while (0xff == g_sd_firstByteResponse); // wait for transmission end

// Ensure this response is "active"
	if (SD_RESPONSE_ACTIVE == g_sd_firstByteResponse) {
		// Ignore blank data again
		timeout = SD_RESPONSE_TIMEOUT + CNT;
		do {
			if (err = SPIShiftIn(8, SD_SPI_MODE_IN, dat, SD_SPI_BYTE_IN_SZ))
				return err;

			// Check for timeout
			if ((timeout - CNT) < SD_WIGGLE_ROOM)
				return SD_READ_TIMEOUT;
		} while (SD_DATA_START_ID != *dat); // wait for transmission end

		// Check for the data start identifier and continue reading data
		if (SD_DATA_START_ID == *dat) {
			// Read in requested data bytes
#if (defined SPI_FAST_SECTOR)
			if (SD_SECTOR_SIZE == bytes) {
				SPIShiftIn_sector(dat, 1);
				bytes = 0;
			}
#endif
			while (bytes--) {
#if (defined SD_DEBUG)
				if (err = SPIShiftIn(8, SD_SPI_MODE_IN, dat++, SD_SPI_BYTE_IN_SZ))
					return err;
#elif (defined SPI_FAST)
				SPIShiftIn_fast(8, SD_SPI_MODE_IN, dat++, SD_SPI_BYTE_IN_SZ);
#else
				SPIShiftIn(8, SD_SPI_MODE_IN, dat++, SD_SPI_BYTE_IN_SZ);
#endif
			}

			// Read two more bytes for checksum - throw away data
			for (i = 0; i < 2; ++i) {
				timeout = SD_RESPONSE_TIMEOUT + CNT;
				do {
					if (err = SPIShiftIn(8, SD_SPI_MODE_IN, &checksum, SD_SPI_BYTE_IN_SZ))
						return err;

					// Check for timeout
					if ((timeout - CNT) < SD_WIGGLE_ROOM)
						return SD_READ_TIMEOUT;
				} while (0xff == checksum); // wait for transmission end
			}

			// Send final 0xff
			if (err = SPIShiftOut(8, 0xff, SD_SPI_MODE_OUT))
				return err;
		} else {
			printf("Invalid start ID: 0x%02X\n", *dat);
			return SD_INVALID_DAT_STRT_ID;
		}
	} else
		return SD_INVALID_RESPONSE;

	return 0;
}

uint8 SDWriteBlock (uint16 bytes, uint8 *dat) {
	uint8 err;
	uint32 timeout;

// Read first byte - the R1 response
	timeout = SD_RESPONSE_TIMEOUT + CNT;
	do {
		if (err = SPIShiftIn(8, SD_SPI_MODE_IN, &g_sd_firstByteResponse,
				SD_SPI_BYTE_IN_SZ))
			return err;

		// Check for timeout
		if (0 < (timeout - CNT) && (timeout - CNT) < SD_WIGGLE_ROOM)
			return SD_READ_TIMEOUT;
	} while (0xff == g_sd_firstByteResponse); // wait for transmission end

// Ensure this response is "active"
	if (SD_RESPONSE_ACTIVE == g_sd_firstByteResponse) {
		// Received "active" response

		// Send data Start ID
		if (err = SPIShiftOut(8, SD_DATA_START_ID, SD_SPI_MODE_OUT))
			return err;

		// Send all bytes
		while (bytes--) {
#if (defined SD_DEBUG)
			if (err = SPIShiftOut(8, *(dat++), SD_SPI_MODE_OUT))
				return err;
#elif (defined SPI_FAST)
			SPIShiftOut_fast(8, *(dat++), SD_SPI_MODE_OUT);
#else
			SPIShiftOut(8, *(dat++), SD_SPI_MODE_OUT);
#endif
		}

		// Receive and digest response token
		timeout = SD_RESPONSE_TIMEOUT + CNT;
		do {
			if (err = SPIShiftIn(8, SD_SPI_MODE_IN, &g_sd_firstByteResponse,
					SD_SPI_BYTE_IN_SZ))
				return err;

			// Check for timeout
			if (0 < (timeout - CNT) && (timeout - CNT) < SD_WIGGLE_ROOM)
				return SD_READ_TIMEOUT;
		} while (0xff == g_sd_firstByteResponse); // wait for transmission end
		if (SD_RSPNS_TKN_ACCPT != (g_sd_firstByteResponse & (uint8) SD_RSPNS_TKN_BITS))
			return SD_INVALID_RESPONSE;
	}

	return 0;
}

uint8 SDReadDataBlock (uint32 address, uint8 *dat) {
	uint8 err;
	uint8 temp = 0;

// Wait until the SD card is no longer busy
	while (!temp)
		SPIShiftIn(8, SD_SPI_MODE_IN, &temp, 1);

#if (defined SD_DEBUG && defined SD_VERBOSE)
	printf("Reading block at sector address: 0x%08X / %u\n", address, address);
#endif

	GPIOPinClear(g_sd_cs);
	if (err = SDSendCommand(SD_CMD_RD_BLOCK, address, SD_CRC_OTHER))
		return err;

	if (err = SDReadBlock(SD_SECTOR_SIZE, dat))
		return err;
	GPIOPinSet(g_sd_cs);

	return 0;
}

uint8 SDWriteDataBlock (uint32 address, uint8 *dat) {
	uint8 err;
	uint8 temp = 0;

// Wait until the SD card is no longer busy
	while (!temp)
		SPIShiftIn(8, SD_SPI_MODE_IN, &temp, 1);

#if (defined SD_DEBUG && defined SD_VERBOSE)
	printf("Writing block at sector address: 0x%08X / %u\n", address, address);
#endif

	GPIOPinClear(g_sd_cs);
	if (err = SDSendCommand(SD_CMD_WR_BLOCK, address, SD_CRC_OTHER))
		return err;

	if (err = SDWriteBlock(SD_SECTOR_SIZE, dat))
		return err;
	GPIOPinSet(g_sd_cs);

	return 0;
}

uint16 SDReadDat16 (const uint8 buf[]) {
	return (buf[1] << 8) + buf[0];
}

uint32 SDReadDat32 (const uint8 buf[]) {
	return (buf[3] << 24) + (buf[2] << 16) + (buf[1] << 8) + buf[0];
}

#ifdef SD_FILE_WRITE
void SDWriteDat16 (uint8 buf[], const uint16 dat) {
	buf[1] = (uint8) (dat >> 8);
	buf[0] = (uint8) dat;
}

void SDWriteDat32 (uint8 buf[], const uint32 dat) {
	buf[3] = (uint8) (dat >> 24);
	buf[2] = (uint8) (dat >> 16);
	buf[1] = (uint8) (dat >> 8);
	buf[0] = (uint8) dat;
}
#endif

uint32 SDGetSectorFromPath (const char *path) {
// TODO: Return an actual path

	/*if ('/' == path[0]) {
	 } // Start from the root address
	 else {
	 } // Start from the current directory*/

	return g_sd_rootAddr;
}

uint32 SDGetSectorFromAlloc (uint32 allocUnit) {
	if (SD_FAT_32 == g_sd_filesystem)
		allocUnit -= g_sd_rootAllocUnit;
	else
		allocUnit -= 2;
	allocUnit *= 8;
	allocUnit += g_sd_firstDataAddr;
	return allocUnit;
}

uint8 SDGetFATValue (const uint32 fatEntry, uint32 *value) {
	uint8 err;
	uint32 firstAvailableAllocUnit;

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Reading from the FAT...\n");
	printf("\tLooking for entry: 0x%08X / %u\n", fatEntry, fatEntry);
#endif

// Do we need to load a new fat sector?
	if ((fatEntry >> g_sd_entriesPerFatSector_Shift) != g_sd_curFatSector) {
#ifdef SD_FILE_WRITE
		// If the currently loaded FAT sector has been modified, save it
		if (g_sd_fatMod) {
			SDWriteDataBlock(g_sd_curFatSector + g_sd_fatStart, g_sd_fat);
			SDWriteDataBlock(g_sd_curFatSector + g_sd_fatStart + g_sd_fatSize, g_sd_fat);
			g_sd_fatMod = 0;
		}
#endif
		// Need new sector, load it
		g_sd_curFatSector = fatEntry >> g_sd_entriesPerFatSector_Shift;
		if (err = SDReadDataBlock(g_sd_curFatSector + g_sd_fatStart, g_sd_fat))
			return err;
#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
		SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
#endif
	}
	firstAvailableAllocUnit = g_sd_curFatSector << g_sd_entriesPerFatSector_Shift;

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("\tLooks like I need FAT sector: 0x%08X / %u\n", g_sd_curFatSector,
			g_sd_curFatSector);
	printf("\tWith an offset of: 0x%04X / %u\n",
			(fatEntry - firstAvailableAllocUnit) << 2,
			(fatEntry - firstAvailableAllocUnit) << 2);
#endif

// The necessary FAT sector has been loaded and the next allocation unit is known,
// proceed with loading the next data sector and incrementing the cluster variables

// Retrieve the next allocation unit number
	if (SD_FAT_16 == g_sd_filesystem)
		*value = SDReadDat16(&g_sd_fat[(fatEntry - firstAvailableAllocUnit) << 1]);
	else
		/* Implied check for (SD_FAT_32 == g_sd_filesystem) */
		*value = SDReadDat32(&g_sd_fat[(fatEntry - firstAvailableAllocUnit) << 2]);
	*value &= 0x0FFFFFFF; // Clear the highest 4 bits - they are always reserved
#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("\tReceived value: 0x%08X / %u\n", *value, *value);
#endif

	return 0;
}

uint8 SDLoadNextSector (sd_buffer *buf) {
	uint8 err;

#ifdef SD_FILE_WRITE
	if (buf->mod)
		SDWriteDataBlock(buf->curClusterStartAddr + buf->curSectorOffset, buf->buf);
#endif

// Check for the end-of-chain marker (end of file)
	if (((uint32) SD_EOC_BEG) <= buf->nextAllocUnit)
		return SD_EOC_END;

// Are we looking at the root directory of a FAT16 system?
	if (SD_FAT_16 == g_sd_filesystem && g_sd_rootAddr == (buf->curClusterStartAddr)) {
		// Root dir of FAT16; Is it the last sector in the root directory?
		if (g_sd_rootDirSectors == (buf->curSectorOffset))
			return SD_EOC_END;
		// Root dir of FAT16; Not last sector
		else
			return SDReadDataBlock(++(buf->curSectorOffset), buf->buf); // Any error from reading the data block will be returned to calling function
	}
// We are looking at a generic data cluster.
	else {
		// Gen. data cluster; Have we reached the end of the cluster?
		if (((1 << g_sd_sectorsPerCluster_shift) - 1) > (buf->curSectorOffset)) {
			// Gen. data cluster; Not the end; Load next sector in the cluster
			return SDReadDataBlock(++(buf->curSectorOffset) + buf->curClusterStartAddr,
					buf->buf); // Any error from reading the data block will be returned to calling function
		}
		// End of generic data cluster; Look through the FAT to find the next cluster
		else
			return SDIncCluster(buf);
	}

#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
	printf("New sector loaded:\n");
	SDPrintHexBlock(buf->buf, SD_SECTOR_SIZE);
	putchar('\n');
#endif

	return 0;
}

uint8 SDLoadSectorFromOffset (sd_file *f, const uint32 offset) {
	uint8 err;
	uint32 clusterOffset = offset >> g_sd_sectorsPerCluster_shift;

	// Find the correct cluster
	if (f->curCluster < clusterOffset) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Need to fast-forward through the FAT to find the cluster\n");
#endif
		// Desired cluster comes after the currently loaded one - this is easy and requires
		// continuing to look forward through the FAT from the current position
		clusterOffset -= f->curCluster;
		while (clusterOffset--) {
			++(f->curCluster);
			f->buf->curAllocUnit = f->buf->nextAllocUnit;
			if (err = SDGetFATValue(f->buf->curAllocUnit, &(f->buf->nextAllocUnit)))
				return err;
		}
		f->buf->curClusterStartAddr = SDGetSectorFromAlloc(f->buf->curAllocUnit);
	} else if (f->curCluster > clusterOffset) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("Need to backtrack through the FAT to find the cluster\n");
#endif
		// Desired cluster is an earlier cluster than the currently loaded one - this requires
		// starting from the beginning and working forward
		f->buf->curAllocUnit = f->firstAllocUnit;
		if (err = SDGetFATValue(f->buf->curAllocUnit, &(f->buf->nextAllocUnit)))
			return err;
		f->curCluster = 0;
		while (clusterOffset--) {
			++(f->curCluster);
			f->buf->curAllocUnit = f->buf->nextAllocUnit;
			if (err = SDGetFATValue(f->buf->curAllocUnit, &(f->buf->nextAllocUnit)))
				return err;
		}
		f->buf->curClusterStartAddr = SDGetSectorFromAlloc(f->buf->curAllocUnit);
	}

// Followed by finding the correct sector
	f->buf->curSectorOffset = offset % (1 << g_sd_sectorsPerCluster_shift);
	f->curSector = offset;

#ifdef SD_FILE_WRITE
	if (f->buf->mod) {
		SDWriteDataBlock(f->buf->curClusterStartAddr + f->buf->curSectorOffset,
				f->buf->buf);
		f->buf->mod = 0;
	}
#endif
	SDReadDataBlock(f->buf->curClusterStartAddr + f->buf->curSectorOffset, f->buf->buf);

	return 0;
}

uint8 SDIncCluster (sd_buffer *buf) {
	uint8 err;

// Update g_sd_cur*
	buf->curAllocUnit = buf->nextAllocUnit;
	if (err = SDGetFATValue(buf->curAllocUnit, &(buf->nextAllocUnit)))
		return err;
	buf->curClusterStartAddr = SDGetSectorFromAlloc(buf->curAllocUnit);
	buf->curSectorOffset = 0;

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Incrementing the cluster. New parameters are:\n");
	printf("\tCurrent allocation unit: 0x%08X / %u\n", buf->curAllocUnit,
			buf->curAllocUnit);
	printf("\tNext allocation unit: 0x%08X / %u\n", buf->nextAllocUnit,
			buf->nextAllocUnit);
	printf("\tCurrent cluster starting address: 0x%08X / %u\n", buf->curClusterStartAddr,
			buf->curClusterStartAddr);
#endif

#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
	if (err = SDReadDataBlock(buf->curClusterStartAddr, buf->buf))
	return err;
	SDPrintHexBlock(buf->buf, SD_SECTOR_SIZE);
	return 0;
#else
	return SDReadDataBlock(buf->curClusterStartAddr, buf->buf);
#endif
}

uint8 SDReloadBuf (sd_file *f) {
	uint8 err;

	// Function is only called if it has already been determined that the buffer needs to
	// be loaded - no checks need to be run

	// If the currently loaded buffer has been modified, save it
	if (f->buf->mod) {
		if (err = SDWriteDataBlock(f->buf->curClusterStartAddr + f->buf->curSectorOffset,
				f->buf->buf))
			return err;
		f->buf->mod = 0;
	}

	// Set current values to show that the first sector of the file is loaded.
	// SDLoadSectorFromOffset() loads the sector unconditionally before returning so we
	// do not need to load the sector here
	f->buf->curAllocUnit = f->firstAllocUnit;
	f->buf->curClusterStartAddr = SDGetSectorFromAlloc(f->firstAllocUnit);
	f->buf->curSectorOffset = 0;
	if (err = SDGetFATValue(f->firstAllocUnit, &(f->buf->nextAllocUnit)))
		return err;

	// Proceed with loading the sector
	if (err = SDLoadSectorFromOffset(f, f->curSector))
		return err;
	f->buf->id = f->id;

	return 0;
}
void SDGetFilename (const uint8 *buf, char *filename) {
	uint8 i, j = 0;

// Read in the first 8 characters - stop when a space is reached or 8 characters have been read, whichever comes first
	for (i = 0; i < SD_FILE_NAME_LEN; ++i) {
		if (0x05 == buf[i])
			filename[j++] = 0xe5;
		else if (' ' != buf[i])
			filename[j++] = buf[i];
	}

// Determine if there is more past the first 8 - Again, stop when a space is reached
	if (' ' != buf[SD_FILE_NAME_LEN]) {
		filename[j++] = '.';
		for (i = SD_FILE_NAME_LEN; i < SD_FILE_NAME_LEN + SD_FILE_EXTENSION_LEN; ++i) {
			if (' ' != buf[i])
				filename[j++] = buf[i];
		}
	}

// Insert null-terminator
	filename[j] = 0;
}

uint8 SDFind (const char *filename, uint16 *fileEntryOffset) {
	uint8 err;
	char readEntryName[SD_FILENAME_STR_LEN];

	// Save the current buffer
	if (g_sd_buf.mod) {
		if (err = SDWriteDataBlock(
				g_sd_buf.curClusterStartAddr + g_sd_buf.curSectorOffset, g_sd_buf.buf))
			return err;
		g_sd_buf.mod = 0;
	}

	*fileEntryOffset = 0;

	// If we aren't looking at the beginning of the directory cluster, we must backtrack
	// to the beginning and then begin listing files
	if (g_sd_buf.curSectorOffset
			|| (SDGetSectorFromAlloc(g_sd_dir_firstAllocUnit)
					!= g_sd_buf.curClusterStartAddr)) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
		printf("'find' requires a backtrack to beginning of directory's cluster\n");
#endif
		g_sd_buf.curClusterStartAddr = SDGetSectorFromAlloc(g_sd_dir_firstAllocUnit);
		g_sd_buf.curSectorOffset = 0;
		g_sd_buf.curAllocUnit = g_sd_dir_firstAllocUnit;
		if (err = SDGetFATValue(g_sd_buf.curAllocUnit, &g_sd_buf.nextAllocUnit))
			return err;
		if (err = SDReadDataBlock(g_sd_buf.curClusterStartAddr, g_sd_buf.buf))
			return err;
	}
	g_sd_buf.id = SD_FOLDER_ID;

// Loop through all entries in the current directory until we find the correct one
// Function will exit normally with SD_EOC_END error code if the file is not found
	while (g_sd_buf.buf[*fileEntryOffset]) {
		// Check if file is valid, retrieve the name if it is
		if (!(SD_DELETED_FILE_MARK == g_sd_buf.buf[*fileEntryOffset])) {
			SDGetFilename(&(g_sd_buf.buf[*fileEntryOffset]), readEntryName);
			if (!strcmp(filename, readEntryName))
				return 0;	// File names match, return 0 to indicate a successful search
		}

		// Increment to the next file
		*fileEntryOffset += SD_FILE_ENTRY_LENGTH;

		// If it was the last entry in this sector, proceed to the next one
		if (SD_SECTOR_SIZE == *fileEntryOffset) {
			// Last entry in the sector, attempt to load a new sector
			// Possible error value includes end-of-chain marker
			if (err = SDLoadNextSector(&g_sd_buf))
				return err;

			*fileEntryOffset = 0;
		}
	}

	return SD_FILENAME_NOT_FOUND;
}

#ifdef SD_FILE_WRITE
uint32 SDFindEmptySpace (const uint8 restore) {
	uint8 i;
	uint32 alloc = g_sd_curFatSector >> g_sd_entriesPerFatSector_Shift;
	uint16 allocOffset = 0;
	uint32 fatSectorAddr = g_sd_curFatSector;
	uint32 retVal;
	// NOTE: g_sd_curFatSector is not modified until end of function - it is used throughout
	// this function as the original starting point

#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
	printf(
			"\n*** SDFindEmptySpace() initialized with FAT sector 0x%08X / %u loaded ***\n",
			g_sd_curFatSector, g_sd_curFatSector);
	SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
#endif

	// Find the first empty allocation unit and write the EOC marker
	if (SD_FAT_16 == g_sd_filesystem) {
		// Loop until we find an empty cluster
		while (SDReadDat16(&(g_sd_fat[allocOffset]))) {
#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
			printf("Searching the following sector...\n");
			SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
#endif
			// Stop when we either reach the end of the current block or find an empty cluster
			while (SDReadDat16(&(g_sd_fat[allocOffset])) && (SD_SECTOR_SIZE > allocOffset))
				allocOffset += SD_FAT_16;
			// If we reached the end of a sector...
			if (SD_SECTOR_SIZE <= allocOffset) {
				// If the currently loaded FAT sector has been modified, save it
				if (g_sd_fatMod) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
					printf("FAT sector has been modified; saving now... ");
#endif
					SDWriteDataBlock(g_sd_curFatSector, g_sd_fat);
					SDWriteDataBlock(g_sd_curFatSector + g_sd_fatSize, g_sd_fat);
#if (defined SD_VERBOSE && defined SD_DEBUG)
					printf("done!\n");
#endif
					g_sd_fatMod = 0;
				}
				// Read the next fat sector
				SDReadDataBlock(++fatSectorAddr, g_sd_fat);
			}
		}
		SDWriteDat16(g_sd_fat + allocOffset, (uint16) SD_EOC_END);
		g_sd_fatMod = 1;
	} else /* Implied and not needed: "if (SD_FAT_32 == g_sd_filesystem)" */{
		// Loop until we find an empty cluster
		while (SDReadDat32(&(g_sd_fat[allocOffset])) & 0x0fffffff) {
#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
			printf("Searching the following sector...\n");
			SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
#endif
			// Stop when we either reach the end of the current block or find an empty cluster
			while ((SDReadDat32(&(g_sd_fat[allocOffset])) & 0x0fffffff)
					&& (SD_SECTOR_SIZE > allocOffset))
				allocOffset += SD_FAT_32;
			// If we reached the end of a sector...
			if (SD_SECTOR_SIZE <= allocOffset) {
				if (g_sd_fatMod) {
#if (defined SD_VERBOSE && defined SD_DEBUG)
					printf("FAT sector has been modified; saving now... ");
#endif
					SDWriteDataBlock(g_sd_curFatSector, g_sd_fat);
					SDWriteDataBlock(g_sd_curFatSector + g_sd_fatSize, g_sd_fat);
#if (defined SD_VERBOSE && defined SD_DEBUG)
					printf("done!\n");
#endif
					g_sd_fatMod = 0;
				}
				// Read the next fat sector
				SDReadDataBlock(++fatSectorAddr, g_sd_fat);
			}
		}
		SDWriteDat32(g_sd_fat + allocOffset, (uint32) SD_EOC_END);
		g_sd_fatMod = 1;
	}

#if (defined SD_VERBOSE && defined SD_DEBUG)
	printf("Available space found: 0x%08X / %u\n",
			(g_sd_curFatSector << g_sd_entriesPerFatSector_Shift)
					+ allocOffset / g_sd_filesystem,
			(g_sd_curFatSector << g_sd_entriesPerFatSector_Shift)
					+ allocOffset / g_sd_filesystem);
#endif

	// If we loaded a new fat sector (and then modified it directly above), write the
	// sector before re-loading the original
	if (fatSectorAddr != g_sd_curFatSector) {
		SDWriteDataBlock(fatSectorAddr, g_sd_fat);
		SDWriteDataBlock(fatSectorAddr + g_sd_fatSize, g_sd_fat);
		g_sd_fatMod = 0;
		if (restore)
			SDReadDataBlock(g_sd_curFatSector + g_sd_fatStart, g_sd_fat);
		else {
			g_sd_curFatSector = fatSectorAddr - g_sd_fatStart;
		}
	}

	// Return new address to end-of-chain
	retVal = g_sd_curFatSector << g_sd_entriesPerFatSector_Shift;
	retVal += allocOffset / g_sd_filesystem;
	return retVal;
}

uint8 SDExtendFAT (const sd_buffer *buf) {
	uint8 err;
	uint32 newAllocUnit;
#ifdef SD_DEBUG
	if (SD_EOC_END != g_sd_fat[buf->curAllocUnit])
		return SD_INVALID_FAT_APPEND;
	else {
#endif
		// Do we need to load a new fat sector?
		if ((buf->curAllocUnit >> g_sd_entriesPerFatSector_Shift) != g_sd_curFatSector) {
			// Need new sector, load it

		}

		g_sd_curFatSector = buf->curAllocUnit >> g_sd_entriesPerFatSector_Shift;
		if (err = SDReadDataBlock(g_sd_curFatSector + g_sd_fatStart, g_sd_fat))
			return err;
#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
		SDPrintHexBlock(g_sd_fat, SD_SECTOR_SIZE);
#endif

		newAllocUnit = SDFindEmptySpace(1);

		if (SD_FAT_16 == g_sd_filesystem) {
			SDWriteDat16(
					&(g_sd_fat[buf->curAllocUnit % (1 << g_sd_entriesPerFatSector_Shift)]),
					(uint16) newAllocUnit);
		} else {
			SDWriteDat32(
					&(g_sd_fat[buf->curAllocUnit % (1 << g_sd_entriesPerFatSector_Shift)]),
					newAllocUnit);
		}
		g_sd_fatMod = 1;
#ifdef SD_DEBUG
	}
#endif

	return 0;
}

uint8 SDCreateFile (const char *name, const uint16 *fileEntryOffset) {
	uint8 err, i, j;
	char uppercaseName[SD_FILENAME_STR_LEN];
	uint32 allocUnit;

#ifdef SD_DEBUG
#ifdef SD_VERBOSE
	printf("Creating new file: %s\n", name);
#endif
// Parameter checking...
	if (SD_FILENAME_STR_LEN < strlen(name))
		return SD_INVALID_FILENAME;
#endif

	// Convert the name to uppercase
	for (i = 0; name[i]; ++i)
		if ('a' <= name[i] && 'z' >= name[i])
			uppercaseName[i] = name[i] + 'A' - 'a';
		else
			uppercaseName[i] = name[i];

	// Write the file fields in order...

	/* 1) Short file name */
	// Write first section
	for (i = 0; '.' != name[i] && 0 != name[i]; ++i)
		g_sd_buf.buf[*fileEntryOffset + i] = name[i];
	// Check if there is an extension
	if (name[i]) {
		// There might be an extension - pad first name with spaces
		for (j = i; j < SD_FILE_NAME_LEN; ++j)
			g_sd_buf.buf[*fileEntryOffset + j] = ' ';
		// Check if there is a period, as one would expect for a file name with an extension
		if ('.' == name[i]) {
			// Extension exists, write it
			++i;			// Skip the period
			// Insert extension, character-by-character
			for (j = SD_FILE_NAME_LEN; name[i]; ++j)
				g_sd_buf.buf[*fileEntryOffset + j] = name[i++];
			// Pad extension with spaces
			for (; j < SD_FILE_NAME_LEN + SD_FILE_EXTENSION_LEN; ++j)
				g_sd_buf.buf[*fileEntryOffset + j] = ' ';
		}
		// If it wasn't a period or null terminator, throw an error
		else
			return SD_INVALID_FILENAME;
	}
	// No extension, pad with spaces
	else
		for (; i < (SD_FILE_NAME_LEN + SD_FILE_EXTENSION_LEN); ++i)
			g_sd_buf.buf[*fileEntryOffset + i] = ' ';

	/* 2) Write attribute field... */
	// TODO: Allow for file attribute flags to be set, such as SD_READ_ONLY, SD_SUB_DIR, etc
	g_sd_buf.buf[*fileEntryOffset + SD_FILE_ATTRIBUTE_OFFSET] = 0;
	g_sd_buf.mod = 1;

#if (defined SD_VERBOSE && defined SD_DEBUG)
	SDPrintFileEntry(&(g_sd_buf.buf[*fileEntryOffset]), uppercaseName);
#endif

#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
	SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
#endif

	/* 3) Find a spot in the FAT (do not check for a full FAT, assume space is available) */
	allocUnit = SDFindEmptySpace(0);
	SDWriteDat16(&(g_sd_buf.buf[*fileEntryOffset + SD_FILE_START_CLSTR_LOW]),
			(uint16) allocUnit);
	if (SD_FAT_32 == g_sd_filesystem)
		SDWriteDat16(&(g_sd_buf.buf[*fileEntryOffset + SD_FILE_START_CLSTR_HIGH]),
				(uint16) (allocUnit >> 16));

	/* 4) Write the size of the file (currently 0) */
	SDWriteDat32(&(g_sd_buf.buf[*fileEntryOffset + SD_FILE_LEN_OFFSET]), 0);

#if (defined SD_VERBOSE_BLOCKS && defined SD_VERBOSE && defined SD_DEBUG)
	printf("New file entry at offset 0x%08X / %u looks like...\n", *fileEntryOffset,
			*fileEntryOffset);
	SDPrintHexBlock(g_sd_buf.buf, SD_SECTOR_SIZE);
#endif

	g_sd_buf.mod = 1;

	return 0;
}
#endif

#if (defined SD_SHELL || defined SD_VERBOSE)
inline void SDPrintFileEntry (const uint8 *file, char filename[]) {
	SDPrintFileAttributes(file[SD_FILE_ATTRIBUTE_OFFSET]);
	SDGetFilename(file, filename);
	printf("\t\t%s", filename);
	if (SD_SUB_DIR & file[SD_FILE_ATTRIBUTE_OFFSET])
		putchar('/');
	putchar('\n');
}

void SDPrintFileAttributes (const uint8 flag) {
// Print file attributes
	if (SD_READ_ONLY & flag)
		putchar(SD_READ_ONLY_CHAR);
	else
		putchar(SD_READ_ONLY_CHAR_);

	if (SD_HIDDEN_FILE & flag)
		putchar(SD_HIDDEN_FILE_CHAR);
	else
		putchar(SD_HIDDEN_FILE_CHAR_);

	if (SD_SYSTEM_FILE & flag)
		putchar(SD_SYSTEM_FILE_CHAR);
	else
		putchar(SD_SYSTEM_FILE_CHAR_);

	if (SD_VOLUME_ID & flag)
		putchar(SD_VOLUME_ID_CHAR);
	else
		putchar(SD_VOLUME_ID_CHAR_);

	if (SD_SUB_DIR & flag)
		putchar(SD_SUB_DIR_CHAR);
	else
		putchar(SD_SUB_DIR_CHAR_);

	if (SD_ARCHIVE & flag)
		putchar(SD_ARCHIVE_CHAR);
	else
		putchar(SD_ARCHIVE_CHAR_);
}
#endif

#ifdef SD_DEBUG
void SDError (const uint8 err) {
	char str[] = "SD Error %u: %s\n";

	switch (err) {
		case SD_INVALID_CMD:
			printf(str, (err - SD_ERRORS_BASE), "Invalid command");
			break;
		case SD_READ_TIMEOUT:
			printf(str, (err - SD_ERRORS_BASE), "Timed out during read");
			printf("\nRead sector address was: 0x%08X / %u", g_sd_sectorRdAddress,
					g_sd_sectorRdAddress);
			break;
		case SD_INVALID_NUM_BYTES:
			printf(str, (err - SD_ERRORS_BASE), "Invalid number of bytes");
			break;
		case SD_INVALID_RESPONSE:
#ifdef SD_VERBOSE
			printf("SD Error %u: %s0x%02X\nThe following bits are set:\n",
					(err - SD_ERRORS_BASE), "Invalid first-byte response\n\tReceived: ",
					g_sd_firstByteResponse);
#else
			printf("SD Error %u: %s%u\n", (err - SD_ERRORS_BASE),
					"Invalid first-byte response\n\tReceived: ", g_sd_firstByteResponse);
#endif
			SDFirstByteExpansion(g_sd_firstByteResponse);
			break;
		case SD_INVALID_INIT:
#ifdef SD_VERBOSE
			printf("SD Error %u: %s\n\tResponse: 0x%02X\n", (err - SD_ERRORS_BASE),
					"Invalid response during initialization", g_sd_firstByteResponse);
#else
			printf("SD Error %u: %s\n\tResponse: %u\n", (err - SD_ERRORS_BASE),
					"Invalid response during initialization", g_sd_firstByteResponse);
#endif
			break;
		case SD_INVALID_FILESYSTEM:
			printf(str, (err - SD_ERRORS_BASE), "Invalid filesystem");
			break;
		case SD_INVALID_DAT_STRT_ID:
#ifdef SD_VERBOSE
			printf("SD Error %u: %s0x%02X\n", (err - SD_ERRORS_BASE),
					"Invalid data-start ID\n\tReceived: ", g_sd_firstByteResponse);
#else
			printf("SD Error %u: %s%u\n", (err - SD_ERRORS_BASE),
					"Invalid data-start ID\n\tReceived: ", g_sd_firstByteResponse);
#endif
			break;
		case SD_FILENAME_NOT_FOUND:
			printf(str, (err - SD_ERRORS_BASE), "Filename not found");
			break;
		case SD_EMPTY_FAT_ENTRY:
			printf(str, (err - SD_ERRORS_BASE), "FAT points to empty entry");
			break;
		case SD_CORRUPT_CLUSTER:
			printf(str, (err - SD_ERRORS_BASE), "SD cluster is corrupt");
			break;
		case SD_INVALID_PTR_ORIGIN:
			printf(str, (err - SD_ERRORS_BASE), "Invalid pointer origin");
			break;
		case SD_ENTRY_NOT_FILE:
			printf(str, (err - SD_ERRORS_BASE), "Requested file entry is not a file");
			break;
		case SD_INVALID_FILENAME:
			printf(str, (err - SD_ERRORS_BASE),
					"Invalid filename - please use 8.3 format");
			break;
		case SD_INVALID_FAT_APPEND:
			printf(str, (err - SD_ERRORS_BASE),
					"FAT entry append was attempted unnecessarily");
			break;
		case SD_FILE_ALREADY_EXISTS:
			printf(str, (err - SD_ERRORS_BASE),
					"Attempting to create an already existing file");
			break;
		case SD_INVALID_FILE_MODE:
			printf(str, (err - SD_ERRORS_BASE), "Invalid file mode");
			break;
		case SD_TOO_MANY_FATS:
			printf(str, (err - SD_ERRORS_BASE),
					"This driver is only capable of writing files on FAT partitions with 2 copies of the FAT");
			break;
		default:
// Is the error an SPI error?
			if (err > SD_ERRORS_BASE && err < (SD_ERRORS_BASE + SD_ERRORS_LIMIT))
				printf("Unknown SD error %u\n", (err - SD_ERRORS_BASE));
// If not, print unknown error
			else
				printf("Unknown error %u\n", err);
			break;
	}
	while (1)
		;
}

void SDFirstByteExpansion (const uint8 response) {
	if (BIT_0 & response)
		printf("\t0: Idle\n");
	if (BIT_1 & response)
		printf("\t1: Erase reset\n");
	if (BIT_2 & response)
		printf("\t2: Illegal command\n");
	if (BIT_3 & response)
		printf("\t3: Communication CRC error\n");
	if (BIT_4 & response)
		printf("\t4: Erase sequence error\n");
	if (BIT_5 & response)
		printf("\t5: Address error\n");
	if (BIT_6 & response)
		printf("\t6: Parameter error\n");
	if (BIT_7 & response)
		printf("\t7: Something is really screwed up. This should always be 0.\n");
}
#endif
