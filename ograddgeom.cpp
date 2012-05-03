/******************************************************************************
 *
 * Project:  ograddgeom - GDAL/OGR Utilities
 * Purpose:  Add geometry fields (area, perimeter, ...) to an OGR polygon layer
 * Author:   Chris Toney (christoney@fs.fed.us)
 *
 ******************************************************************************/

#include "ogr_api.h"
#include "ogrsf_frmts.h"
#include "ogr_p.h"
#include <math.h>

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/

static void Usage()

{
    printf("Usage: ograddgeom datasource_name polygon_layer\n");
	printf("Attempts to add fields AREA, PERIMETER to a polygon layer.\n");
	printf("\n");
    exit(1);
}


/************************************************************************/
/*                                main()                                */
/************************************************************************/

int main(int nArgc, char ** papszArgv)
{
    const char  *pszDataSource = NULL;
    const char  *pszLayer = NULL;

    OGRRegisterAll();


/* -------------------------------------------------------------------- */
/*      Process command line arguments.                                 */
/* -------------------------------------------------------------------- */
    nArgc = OGRGeneralCmdLineProcessor(nArgc, &papszArgv, 0);
    
    if(nArgc < 1)
        exit(-nArgc);

    for(int iArg = 1; iArg < nArgc; iArg++)
    {
        if(pszDataSource == NULL)
            pszDataSource = papszArgv[iArg];
        else if(pszLayer == NULL)
            pszLayer = papszArgv[iArg];
    }

    if( pszDataSource == NULL )
        Usage();


/* -------------------------------------------------------------------- */
/*      Open data source and get the layer.                             */
/* -------------------------------------------------------------------- */
    OGRDataSource		*poDS = NULL;
    OGRSFDriver			*poDriver = NULL;

    poDS = OGRSFDriverRegistrar::Open(pszDataSource, TRUE, &poDriver);
    if(poDS == NULL)
    {
		printf("Failed to open data source with update access.\n");
		exit(1);
    }

	OGRLayer			*poLayer = NULL;

	poLayer = poDS->GetLayerByName(pszLayer);
    if(poLayer == NULL)
    {
		printf("Failed to fetch the layer.\n");
		exit(1);
    }

	if(poLayer->GetGeomType() != wkbPolygon
		&& poLayer->GetGeomType() != wkbMultiPolygon)
	{
		printf( "Layer is not polygon type (%s)\n", 
			OGRGeometryTypeToName(poLayer->GetGeomType()) );
		exit(1);
	}

/* -------------------------------------------------------------------- */
/*      Check if fields exist and create them if needed                 */
/* -------------------------------------------------------------------- */

	OGRFeatureDefn *poFDefn = poLayer->GetLayerDefn();
	int iPolyIdField = -1;
	int iAreaField = -1;
	int iPerimField = -1;
	int iShpIdxField = -1;
	int iFracDimIdxField = -1;

	if(poFDefn->GetFieldIndex("POLYID") < 0)
	{
		OGRFieldDefn oPolyIdField("POLYID", OFTInteger);
		if(poLayer->CreateField(&oPolyIdField) != OGRERR_NONE)
		{
			printf("Creating POLYID field failed.\n");
			exit(1);
		}
	}

	if(poFDefn->GetFieldIndex("AREA") < 0)
	{
		OGRFieldDefn oAreaField("AREA", OFTReal);
		oAreaField.SetPrecision(2);
		if(poLayer->CreateField(&oAreaField) != OGRERR_NONE)
		{
			printf("Creating AREA field failed.\n");
			exit(1);
		}
	}

	if(poFDefn->GetFieldIndex("PERIMETER") < 0)
	{
		OGRFieldDefn oPerimField("PERIMETER", OFTReal);
		oPerimField.SetPrecision(2);
		if(poLayer->CreateField(&oPerimField) != OGRERR_NONE)
		{
			printf("Creating PERIMETER field failed.\n");
			exit(1);
		}
	}

	if(poFDefn->GetFieldIndex("SHPIDX") < 0)
	{
		OGRFieldDefn oShpIdxField("SHPIDX", OFTReal);
		if(poLayer->CreateField(&oShpIdxField) != OGRERR_NONE)
		{
			printf("Creating SHPIDX field failed.\n");
			exit(1);
		}
	}

	if(poFDefn->GetFieldIndex("FRACDIMIDX") < 0)
	{
		OGRFieldDefn oFracDimIdxField("FRACDIMIDX", OFTReal);
		if(poLayer->CreateField(&oFracDimIdxField) != OGRERR_NONE)
		{
			printf("Creating FRACDIMIDX field failed.\n");
			exit(1);
		}
	}

	iPolyIdField = poFDefn->GetFieldIndex("POLYID");
	iAreaField = poFDefn->GetFieldIndex("AREA");
	iPerimField = poFDefn->GetFieldIndex("PERIMETER");
	iShpIdxField = poFDefn->GetFieldIndex("SHPIDX");
	iFracDimIdxField = poFDefn->GetFieldIndex("FRACDIMIDX");

/* -------------------------------------------------------------------- */
/*      Read the features and calculate geometry                        */
/* -------------------------------------------------------------------- */

	OGRFeature		*poFeature;
	OGRGeometry		*poGeom;
	OGRPolygon		*poPoly;
	OGRMultiPolygon	*poMultiPoly;
	OGRLinearRing	*poLinearRing;
	int				iPolyId = 0;
	int				iPoly, iRing;
	double			dfArea, dfPerim, dfShpIdx, dfFracDimIdx;

	poLayer->ResetReading();
	while( (poFeature = poLayer->GetNextFeature()) != NULL)
	{
		poFeature->SetField(iPolyIdField, ++iPolyId);

		poGeom = poFeature->GetGeometryRef();
		
		dfArea = 0.0;
		dfPerim = 0.0;
		dfShpIdx = -1.0;
		dfFracDimIdx = -1.0;

		if(poGeom->getGeometryType() == wkbPolygon)
		{
			poPoly = (OGRPolygon*)poGeom;
			dfArea = poPoly->get_Area();
			poFeature->SetField(iAreaField, dfArea);

			poLinearRing = poPoly->getExteriorRing();
			dfPerim += poLinearRing->get_Length();
		    for(iRing = 0; iRing < poPoly->getNumInteriorRings(); iRing++)
				dfPerim += poPoly->getInteriorRing(iRing)->get_Length();
			poFeature->SetField(iPerimField, dfPerim);

			dfShpIdx = (0.25*dfPerim) / sqrt(dfArea);
			poFeature->SetField(iShpIdxField, dfShpIdx);

			dfFracDimIdx = (2*log(0.25*dfPerim)) / log(dfArea);
			poFeature->SetField(iFracDimIdxField, dfFracDimIdx);

		}
		else if(poGeom->getGeometryType() == wkbMultiPolygon)
		{
			poMultiPoly = (OGRMultiPolygon*)poGeom;
			dfArea = poMultiPoly->get_Area();
			poFeature->SetField(iAreaField, dfArea);

			for(iPoly = 0; iPoly < poMultiPoly->getNumGeometries(); iPoly++)
			{
				poPoly = (OGRPolygon*)poMultiPoly->getGeometryRef(iPoly);

				poLinearRing = poPoly->getExteriorRing();
				dfPerim += poLinearRing->get_Length();
				for(iRing = 0; iRing < poPoly->getNumInteriorRings(); iRing++)
					dfPerim += poPoly->getInteriorRing(iRing)->get_Length();
			}
			poFeature->SetField(iPerimField, dfPerim);

			dfShpIdx = (0.25*dfPerim) / sqrt(dfArea);
			poFeature->SetField(iShpIdxField, dfShpIdx);

			dfFracDimIdx = (2*log(0.25*dfPerim)) / log(dfArea);
			poFeature->SetField(iFracDimIdxField, dfFracDimIdx);
		}
		else
		{
			poFeature->SetField(iAreaField, dfArea);
			poFeature->SetField(iPerimField, dfPerim);
			poFeature->SetField(iShpIdxField, dfShpIdx);
			poFeature->SetField(iFracDimIdxField, dfFracDimIdx);
		}

		poLayer->SetFeature(poFeature);

		OGRFeature::DestroyFeature(poFeature);
	}

/* -------------------------------------------------------------------- */
/*      Clean up								                        */
/* -------------------------------------------------------------------- */

	OGRDataSource::DestroyDataSource(poDS);

	OGRCleanupAll();
}

