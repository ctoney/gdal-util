/******************************************************************************
 *
 * Project:  gdal_combine - GDAL Utilities
 * Purpose:  overlay thematic rasters and find the unique pixel combinations
 * Author:   Chris Toney (christoney@fs.fed.us)
 *
 ******************************************************************************/

#include "gdal.h"
#include "cpl_string.h" 
#include "cpl_hash_set.h"
#include "cpl_conv.h"
#include "ogr_srs_api.h"
#include <time.h>

typedef struct {
	GDALDatasetH hDS;
	GDALRasterBandH hBand;
	GDALDataType eDataType;
	int bIsIntDataType;
	int *panScanline;
	double *padfScanline;
} InputRaster;

typedef struct {
    unsigned int nID;
    unsigned long long nCount;
    char *pszCombination;
} Combination;

unsigned long CombinationHashFunc(const void* elt) {
    Combination* psStruct = (Combination*) elt;
    return CPLHashSetHashStr(psStruct->pszCombination);
}

int CombinationEqualFunc(const void* elt1, const void* elt2) {
    Combination* psStruct1 = (Combination*) elt1;
    Combination* psStruct2 = (Combination*) elt2;
    return strcmp(psStruct1->pszCombination, psStruct2->pszCombination) == 0;
}

void CombinationFreeFunc(void* elt) {
    Combination* psStruct = (Combination*) elt;
    CPLFree(psStruct->pszCombination);
    CPLFree(psStruct);
}

static void Usage() {
    printf( "Usage: gdal_combine [-o out_raster] [-of out_format]\n"
			"       [-ot {Byte/UInt16/UInt32}] [-initid id]\n"
            "       [-co \"NAME=VALUE\"]* [-q]\n"
            "       -csv out_csv_file\n"
			"       [-input_file_list my_list.txt]\n"
            "       [raster files...] \n\n" );
}

/************************************************************************/
/*                        add_file_to_list()                            */
/************************************************************************/

static void add_file_to_list(const char* filename, int* pnInputFiles, 
								char*** pppszInputFilenames)
{
	int nInputFiles = *pnInputFiles;
	char** ppszInputFilenames = *pppszInputFilenames;

	ppszInputFilenames = (char**)CPLRealloc(ppszInputFilenames,
											sizeof(char*) * (nInputFiles+1));
	ppszInputFilenames[nInputFiles++] = CPLStrdup(filename);
	
	*pnInputFiles = nInputFiles;
	*pppszInputFilenames = ppszInputFilenames;
}

/************************************************************************/
/* "itoa" v. 0.4: Written by Lukás Chmela: Released under GPLv3.        */
/*  http://www.strudel.org.uk/itoa/                                     */
/************************************************************************/
char* itoa_(int value, char* result, int base) {
	if (base < 2 || base > 36) {
		*result = '\0';
		return result;
	}

	char* ptr = result, *ptr1 = result, tmp_char;
	int tmp_value;

	do {
		tmp_value = value;
		value /= base;
		*ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" 
			[35 + (tmp_value - value * base)];
	} while (value);

	if (tmp_value < 0) *ptr++ = '-';
	*ptr-- = '\0';
	while(ptr1 < ptr) {
		tmp_char = *ptr;
		*ptr--= *ptr1;
		*ptr1++ = tmp_char;
	}
	return result;
}

/************************************************************************/
/*                          WriteDataToCSVForEach()                     */
/************************************************************************/

static int WriteDataToCSVForEach(void* elt, void* user_data) {
    FILE* fp = (FILE*) user_data;
    Combination* cmb = (Combination*) elt;
	
	//be sure there is no trailing comma in the cmb string
	if(EQUAL(&cmb->pszCombination[strlen(cmb->pszCombination)], ","))
		cmb->pszCombination[strlen(cmb->pszCombination)-1] = '\0';
		
	VSIFPrintf(fp, "%u,%llu,%s\n", cmb->nID, cmb->nCount, cmb->pszCombination);

    return TRUE;
}

/************************************************************************/
/*                           program main                               */
/************************************************************************/

int main(int argc, char ** argv) {

    GDALDatasetH hOutDS;
    GDALDriverH hDriver;
	GDALRasterBandH hOutBand;
	InputRaster *psInputRasters;
	CPLErr eErr;
    int	i, nXoff, nYoff;
    int	nXSize, nYSize;
	const char *pszCSVFile=NULL;
    const char *pszOutRaster=NULL, *pszOutFormat = "GTiff";
	const char *pszOutDataType = NULL;
    GDALDataType eOutDataType = GDT_UInt16;
    double adfGeoTransform[6];
    char **papszCreateOptions = NULL;
	unsigned int *panOutline;
    int nInputFiles = 0;
    char **ppszInputFilenames = NULL;
	int bQuiet = FALSE;
    GDALProgressFunc pfnProgress = GDALTermProgress;
	void* pProgressData = NULL;
	static CPLHashSet* phAllCombination = NULL;
	char szTmp[16];
	unsigned int nCmbID = 0;
	FILE* fp;
	char *pszVarList = NULL;
	int nChar = 0;
	clock_t start, finish;
	double dfDuration;

/* -------------------------------------------------------------------- */
/*      Register standard GDAL drivers and process command options.     */
/* -------------------------------------------------------------------- */
    GDALAllRegister();
	
    argc = GDALGeneralCmdLineProcessor(argc, &argv, 0);
    if(argc < 1)
        exit(-argc);

    for(i = 1; i < argc; i++) {
		if(EQUAL(argv[i],"-o") && i < argc-1)
            pszOutRaster = argv[++i];
			
		else if(EQUAL(argv[i],"-of") && i < argc-1)
            pszOutFormat = argv[++i];
			
		else if(EQUAL(argv[i],"-ot") && i < argc-1) {
            pszOutDataType = argv[++i];
			if(EQUAL(pszOutDataType,"Byte")) {
				eOutDataType = GDT_Byte;
			}
			else if(EQUAL(pszOutDataType,"UInt16")) {
				eOutDataType = GDT_UInt16;
			}
			else if(EQUAL(pszOutDataType,"UInt32")) {
				eOutDataType = GDT_UInt32;
			}
			else {
				fprintf(stderr, "Output data type %s is not valid.\n\n", argv[i]);
				Usage();
				GDALDestroyDriverManager();
				exit(1);
			}
		}

		else if(EQUAL(argv[i],"-initid") && i < argc-1)
            nCmbID = atoi(argv[++i]);
			
		else if(EQUAL(argv[i],"-csv") && i < argc-1)
            pszCSVFile = argv[++i];
			
        else if(EQUAL(argv[i],"-co") && i < argc-1) {
            papszCreateOptions = CSLAddString(papszCreateOptions, argv[++i]);
        }
        else if (EQUAL(argv[i],"-q") || EQUAL(argv[i],"-quiet")) {
            bQuiet = TRUE;
        }
		
        else if(argv[i][0] == '-') {
            fprintf(stderr, "Option %s incomplete, or not recognised.\n\n", argv[i]);
            Usage();
            GDALDestroyDriverManager();
            exit(1);
        }
		
        else if(EQUAL(argv[i],"-input_file_list") && i < argc-1) {
            const char* input_file_list = argv[++i];
            FILE* f = VSIFOpen(input_file_list, "r");
            if (f) {
                while(1) {
                    const char* filename = CPLReadLine(f);
                    if (filename == NULL)
                        break;
                    add_file_to_list(filename, &nInputFiles, 
										&ppszInputFilenames);
                }
                VSIFClose(f);
            }
        }

        else {
            add_file_to_list(argv[i], &nInputFiles, &ppszInputFilenames);
        }
	}
		
    if(pszCSVFile == NULL || nInputFiles == 0) {
        Usage();
		GDALDestroyDriverManager();
		exit(1);
	}
	
	fp = VSIFOpen(pszCSVFile, "wt");
	if(fp == NULL) {
		fprintf(stderr, "Can't open %s for writing output CSV file\n", pszCSVFile);
		GDALDestroyDriverManager();
		exit(1);
	}
	
/* -------------------------------------------------------------------- */
/*      Find the output driver.                                         */
/* -------------------------------------------------------------------- */
    hDriver = GDALGetDriverByName(pszOutFormat);
    if(hDriver == NULL) {
        int	iDr;
        
        printf("Output driver `%s' not recognised.\n", pszOutFormat);
        printf("The following format drivers are configured and support output:\n");
        for(iDr = 0; iDr < GDALGetDriverCount(); iDr++) {
            GDALDriverH hDriver = GDALGetDriver(iDr);

            if(GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATE, NULL) != NULL
                || GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATECOPY, NULL) != NULL) {
                printf("  %s: %s\n", GDALGetDriverShortName(hDriver ),
						GDALGetDriverLongName(hDriver));
            }
        }
        printf("\n");
        Usage();
        GDALDestroyDriverManager();
        exit(1);
    }


    if (!bQuiet) {
		pfnProgress = GDALTermProgress;
		printf("Combining %d input files...\n", nInputFiles);
	}
	
	start = clock();

	psInputRasters = (InputRaster*) CPLMalloc(nInputFiles*sizeof(InputRaster));
	
	for (i=0;i<nInputFiles;i++) {
		psInputRasters[i].hDS = GDALOpen(ppszInputFilenames[i], GA_ReadOnly);
		if(psInputRasters[i].hDS == NULL) {
			fprintf(stderr, "Could not open dataset: %s\n", ppszInputFilenames[i]);
			GDALDestroyDriverManager();
			exit(1);
		}
		
		// TO DO: support specific bands in each raster (for now assume band 1)
		psInputRasters[i].hBand = GDALGetRasterBand(psInputRasters[i].hDS, 1);
		psInputRasters[i].eDataType = GDALGetRasterDataType(psInputRasters[i].hBand);
		
		// is it integer?
		if(psInputRasters[i].eDataType == GDT_Byte || psInputRasters[i].eDataType == GDT_UInt16 ||
			psInputRasters[i].eDataType == GDT_Int16 || psInputRasters[i].eDataType == GDT_UInt32 ||
			psInputRasters[i].eDataType == GDT_Int32) {
			psInputRasters[i].bIsIntDataType = TRUE;
		}
		else {
			psInputRasters[i].bIsIntDataType = FALSE;
		}
	}
	
	//for now all input rasters must have same extent and cell size...
	//use first raster as reference
	nXSize = GDALGetRasterXSize(psInputRasters[0].hDS);
	nYSize = GDALGetRasterYSize(psInputRasters[0].hDS);
	if (!bQuiet)
		printf("raster size: %d x %d\n", nXSize, nYSize);
	
/* -------------------------------------------------------------------- */
/*      Create the output raster if one is requested.                   */
/* -------------------------------------------------------------------- */
	if(pszOutRaster != NULL) {
		hOutDS = GDALCreate(hDriver, pszOutRaster, nXSize, nYSize, 1, 
							eOutDataType, papszCreateOptions);
		if(hOutDS == NULL) {
			fprintf(stderr, "Could not create the output raster\n");
			GDALDestroyDriverManager();
			exit(1);
		}
							
		if(GDALGetGeoTransform(psInputRasters[0].hDS, adfGeoTransform) == CE_None)
			GDALSetGeoTransform(hOutDS, adfGeoTransform);
		if(GDALGetProjectionRef(psInputRasters[0].hDS) != NULL )
			GDALSetProjection(hOutDS, GDALGetProjectionRef(psInputRasters[0].hDS));
		
		hOutBand = GDALGetRasterBand(hOutDS, 1);
	}
	
/* -------------------------------------------------------------------- */
/*      Process the inputs.							                    */
/* -------------------------------------------------------------------- */

	for(i=0;i<nInputFiles;i++) {
		if(psInputRasters[i].bIsIntDataType) {
			psInputRasters[i].panScanline = (int*) VSIMalloc2(nXSize, sizeof(int));
		}
		else {
			//if not integer read as 64-bit float
			psInputRasters[i].padfScanline = (double*) VSIMalloc2(nXSize, sizeof(double));
		}
	}
	
	if(pszOutRaster != NULL)
		panOutline = (unsigned int*) VSIMalloc2(nXSize, sizeof(unsigned int));
	
	phAllCombination = CPLHashSetNew(CombinationHashFunc, CombinationEqualFunc, CombinationFreeFunc);
	
	/* scan input rasters and gather combinations in the hash set */
	for(nYoff=0;nYoff<nYSize;nYoff++) {
		for(i=0;i<nInputFiles;i++) {
			if(psInputRasters[i].bIsIntDataType) {
				GDALRasterIO(psInputRasters[i].hBand, GF_Read, 0, nYoff, nXSize, 1, 
					psInputRasters[i].panScanline, nXSize, 1, GDT_Int32, 0, 0);
			}
			else {
				GDALRasterIO(psInputRasters[i].hBand, GF_Read, 0, nYoff, nXSize, 1, 
					psInputRasters[i].padfScanline, nXSize, 1, GDT_Float64, 0, 0);
			}
		}
		for(nXoff=0;nXoff<nXSize;nXoff++) {
			Combination* cmb = (Combination*) VSIMalloc2(1, sizeof(Combination));
			cmb->pszCombination = (char*) VSIMalloc2(nInputFiles, 16);
			if (cmb == NULL || cmb->pszCombination == NULL) {
				CPLError(CE_Fatal, CPLE_OutOfMemory,
						"VSIMalloc2(): Out of memory. "
						"Can't allocate enough memory to hold all unique combinations\n");
			}
			cmb->pszCombination[0] = '\0';
			
			for(i=0;i<(nInputFiles);i++) {
				if(psInputRasters[i].bIsIntDataType) {
					//itoa is much faster...
					itoa_(psInputRasters[i].panScanline[nXoff], szTmp, 10);
					//sprintf(szTmp, "%ld", psInputRasters[i].panScanline[nXoff]);
				}
				else {
					sprintf(szTmp, "%.f", psInputRasters[i].padfScanline[nXoff]);
				}
				//CPLStrlcat(cmb->pszCombination, szTmp, (nInputFiles * 16));
				strcat(cmb->pszCombination, szTmp);
				if(i < (nInputFiles-1))
					//CPLStrlcat(cmb->pszCombination, ",", (nInputFiles * 16));
					strcat(cmb->pszCombination, ",");
			}
			
			if(Combination* elt = (Combination*) CPLHashSetLookup(phAllCombination, cmb)) {
				cmb->nID = elt->nID;
				cmb->nCount = elt->nCount + 1;
			}
			else {
				cmb->nID = nCmbID++;
				cmb->nCount = 1;
			}
			
			if(pszOutRaster != NULL)
				panOutline[nXoff] = cmb->nID;
			
			//printf("%lu%llu,%s\n", cmb->nID, cmb->nCount, cmb->pszCombination);
			CPLHashSetInsert(phAllCombination, cmb);
		}
		
		//write a line to the output raster if needed
		if(pszOutRaster != NULL) {
			eErr = GDALRasterIO(hOutBand, GF_Write, 0, nYoff, nXSize, 1, panOutline, 
						nXSize, 1, GDT_UInt32, 0, 0);
			//TO DO: if(eErr != CE_None)
		}
						
		if (!bQuiet)
			pfnProgress(nYoff / (nYSize-1.0), NULL, pProgressData);
	}
	
	if(pszOutRaster != NULL && !bQuiet) {
		printf("\nRaster output written to: %s\n", pszOutRaster);

		//warn if the combination ID exceeded the range of eOutDataType
		if(nCmbID >= (1<<GDALGetDataTypeSize(eOutDataType))) {
			printf( "\nWARNING: The number of unique combinations (%u) exceeded "
					"the upper limit (%u) of the output data type. The output "
					"raster contains invalid data.\n\n", 
					nCmbID, (1<<GDALGetDataTypeSize(eOutDataType))-1 );
		}
	}

	/* write the output CSV file */
	for (i=0;i<nInputFiles;i++) {
		nChar = nChar + (strlen(CPLGetBasename(ppszInputFilenames[i])) + 1);
	}
	pszVarList = (char*) CPLMalloc(nChar);
	pszVarList[0] = '\0';
	for (i=0;i<nInputFiles;i++) {
		strcat(pszVarList, CPLGetBasename(ppszInputFilenames[i]));
		if(i < (nInputFiles-1)) 
			strcat(pszVarList, ",");
	}
	VSIFPrintf(fp, "CMB_ID,COUNT,%s\n", pszVarList);
	CPLHashSetForeach(phAllCombination, WriteDataToCSVForEach, fp);
	VSIFClose(fp);
	fp = NULL;
	if (!bQuiet)
		printf("Tabular output written to: %s\n", pszCSVFile);

	finish = clock();
	dfDuration = (double)(finish - start) / CLOCKS_PER_SEC;
	if (!bQuiet)
		printf("gdal_combine completed in %2.1f seconds\n\n", dfDuration);
	
	// clean up
	if(pszOutRaster != NULL)
		GDALClose(hOutDS);
		
	for (i=0;i<nInputFiles;i++) {
		GDALClose(psInputRasters[i].hDS);
		if(psInputRasters[i].bIsIntDataType) {
			CPLFree(psInputRasters[i].panScanline);
		}
		else {
			CPLFree(psInputRasters[i].padfScanline);
		}
		CPLFree(ppszInputFilenames[i]);
	}
	CPLFree(psInputRasters);
    CPLFree(ppszInputFilenames);
	CPLFree(pszVarList);
	
	CPLHashSetDestroy(phAllCombination);
	phAllCombination = NULL;
	
	GDALDestroyDriverManager();
	
	CSLDestroy(argv);
	
    return 0;
}
