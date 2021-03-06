/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRVRTDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2014, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_vrt.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogrwarpedlayer.h"
#include "ogrunionlayer.h"

CPL_CVSID("$Id$");

/************************************************************************/
/*                       OGRVRTGetGeometryType()                        */
/************************************************************************/

typedef struct
{
    OGRwkbGeometryType  eType;
    const char          *pszName;
} OGRGeomTypeName;

static const OGRGeomTypeName asGeomTypeNames[] = { /* 25D versions are implicit */
    { wkbUnknown, "wkbUnknown" },
    { wkbPoint, "wkbPoint" },
    { wkbLineString, "wkbLineString" },
    { wkbPolygon, "wkbPolygon" },
    { wkbMultiPoint, "wkbMultiPoint" },
    { wkbMultiLineString, "wkbMultiLineString" },
    { wkbMultiPolygon, "wkbMultiPolygon" },
    { wkbGeometryCollection, "wkbGeometryCollection" },
    { wkbCircularString, "wkbCircularString" },
    { wkbCompoundCurve, "wkbCompoundCurve" },
    { wkbCurvePolygon, "wkbCurvePolygon" },
    { wkbMultiCurve, "wkbMultiCurve" },
    { wkbMultiSurface, "wkbMultiSurface" },
    { wkbCurve, "wkbCurve" },
    { wkbSurface, "wkbSurface" },
    { wkbNone, "wkbNone" },
    { wkbNone, NULL }
};

OGRwkbGeometryType OGRVRTGetGeometryType(const char* pszGType, int* pbError)
{
    if (pbError)
        *pbError = FALSE;

    OGRwkbGeometryType eGeomType = wkbUnknown;
    int iType;

    for( iType = 0; asGeomTypeNames[iType].pszName != NULL; iType++ )
    {
        if( EQUALN(pszGType, asGeomTypeNames[iType].pszName,
                strlen(asGeomTypeNames[iType].pszName)) )
        {
            eGeomType = asGeomTypeNames[iType].eType;

            if( strstr(pszGType,"25D") != NULL || strstr(pszGType,"Z") != NULL )
                eGeomType = wkbSetZ(eGeomType);
            if( pszGType[strlen(pszGType)-1] == 'M' || pszGType[strlen(pszGType)-2] == 'M' )
                eGeomType = wkbSetM(eGeomType);
            break;
        }
    }

    if( asGeomTypeNames[iType].pszName == NULL )
    {
        if (pbError)
            *pbError = TRUE;
    }

    return eGeomType;
}

/************************************************************************/
/*                          OGRVRTDataSource()                          */
/************************************************************************/

OGRVRTDataSource::OGRVRTDataSource(GDALDriver* poDriverIn) :
    papoLayers(NULL),
    paeLayerType(NULL),
    nLayers(0),
    pszName(NULL),
    psTree(NULL),
    nCallLevel(0),
    poLayerPool(NULL),
    poParentDS(NULL),
    bRecursionDetected(FALSE)
{
    poDriver = poDriverIn;
}

/************************************************************************/
/*                         ~OGRVRTDataSource()                         */
/************************************************************************/

OGRVRTDataSource::~OGRVRTDataSource()

{
    CPLFree( pszName );

    CloseDependentDatasets();

    CPLFree( paeLayerType );

    if( psTree != NULL)
        CPLDestroyXMLNode( psTree );

    delete poLayerPool;
}

/************************************************************************/
/*                        CloseDependentDatasets()                      */
/************************************************************************/

int OGRVRTDataSource::CloseDependentDatasets()
{
    int bHasClosedDependentDatasets = (nLayers > 0);
    for( int i = 0; i < nLayers; i++ )
    {
        delete papoLayers[i];
    }
    CPLFree( papoLayers );
    nLayers = 0;
    papoLayers = NULL;
    return bHasClosedDependentDatasets;
}

/************************************************************************/
/*                        InstantiateWarpedLayer()                      */
/************************************************************************/

OGRLayer*  OGRVRTDataSource::InstantiateWarpedLayer(
                                        CPLXMLNode *psLTree,
                                        const char *pszVRTDirectory,
                                        int bUpdate,
                                        int nRecLevel)
{
    if( !EQUAL(psLTree->pszValue,"OGRVRTWarpedLayer") )
        return NULL;

    OGRLayer* poSrcLayer = NULL;

    for( CPLXMLNode *psSubNode=psLTree->psChild;
         psSubNode != NULL;
         psSubNode=psSubNode->psNext )
    {
        if( psSubNode->eType != CXT_Element )
            continue;

        poSrcLayer = InstantiateLayer(psSubNode, pszVRTDirectory,
                                 bUpdate, nRecLevel + 1);
        if( poSrcLayer != NULL )
            break;
    }

    if( poSrcLayer == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot instantiate source layer" );
        return NULL;
    }

    const char* pszTargetSRS = CPLGetXMLValue(psLTree, "TargetSRS", NULL);
    if( pszTargetSRS == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Missing TargetSRS element within OGRVRTWarpedLayer" );
        delete poSrcLayer;
        return NULL;
    }

    const char* pszGeomFieldName = CPLGetXMLValue(psLTree, "WarpedGeomFieldName", NULL);
    int iGeomField = 0;
    if( pszGeomFieldName != NULL )
    {
        iGeomField = poSrcLayer->GetLayerDefn()->GetGeomFieldIndex(pszGeomFieldName);
        if( iGeomField < 0 )
        {
            CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot find source geometry field '%s'", pszGeomFieldName );
            delete poSrcLayer;
            return NULL;
        }
    }

    OGRSpatialReference* poSrcSRS = NULL;
    const char* pszSourceSRS = CPLGetXMLValue(psLTree, "SrcSRS", NULL);

    if( pszSourceSRS == NULL )
    {
        if( iGeomField < poSrcLayer->GetLayerDefn()->GetGeomFieldCount() )
        {
            poSrcSRS = poSrcLayer->GetLayerDefn()->GetGeomFieldDefn(iGeomField)->GetSpatialRef();
            if( poSrcSRS != NULL)
                poSrcSRS = poSrcSRS->Clone();
        }
    }
    else
    {
        poSrcSRS = new OGRSpatialReference();
        if( poSrcSRS->SetFromUserInput(pszSourceSRS) != OGRERR_NONE )
        {
            delete poSrcSRS;
            poSrcSRS = NULL;
        }
    }

    if( poSrcSRS == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Failed to import source SRS" );
        delete poSrcLayer;
        return NULL;
    }

    OGRSpatialReference* poTargetSRS = new OGRSpatialReference();
    if( poTargetSRS->SetFromUserInput(pszTargetSRS) != OGRERR_NONE )
    {
        delete poTargetSRS;
        poTargetSRS = NULL;
    }

    if( poTargetSRS == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Failed to import target SRS" );
        delete poSrcSRS;
        delete poSrcLayer;
        return NULL;
    }

    if( pszSourceSRS == NULL && poSrcSRS->IsSame(poTargetSRS) )
    {
        delete poSrcSRS;
        delete poTargetSRS;
        return poSrcLayer;
    }

    OGRCoordinateTransformation* poCT =
        OGRCreateCoordinateTransformation( poSrcSRS, poTargetSRS );
    OGRCoordinateTransformation* poReversedCT = (poCT != NULL) ?
        OGRCreateCoordinateTransformation( poTargetSRS, poSrcSRS ) : NULL;

    delete poSrcSRS;
    delete poTargetSRS;

    if( poCT == NULL )
    {
        delete poSrcLayer;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Build the OGRWarpedLayer.                                       */
/* -------------------------------------------------------------------- */

    OGRWarpedLayer* poLayer = new OGRWarpedLayer(poSrcLayer, iGeomField,
                                                 TRUE, poCT, poReversedCT);

/* -------------------------------------------------------------------- */
/*      Set Extent if provided                                          */
/* -------------------------------------------------------------------- */
    const char* pszExtentXMin = CPLGetXMLValue( psLTree, "ExtentXMin", NULL );
    const char* pszExtentYMin = CPLGetXMLValue( psLTree, "ExtentYMin", NULL );
    const char* pszExtentXMax = CPLGetXMLValue( psLTree, "ExtentXMax", NULL );
    const char* pszExtentYMax = CPLGetXMLValue( psLTree, "ExtentYMax", NULL );
    if( pszExtentXMin != NULL && pszExtentYMin != NULL &&
        pszExtentXMax != NULL && pszExtentYMax != NULL )
    {
        poLayer->SetExtent( CPLAtof(pszExtentXMin),
                            CPLAtof(pszExtentYMin),
                            CPLAtof(pszExtentXMax),
                            CPLAtof(pszExtentYMax) );
    }

    return poLayer;
}

/************************************************************************/
/*                        InstantiateUnionLayer()                       */
/************************************************************************/

OGRLayer*  OGRVRTDataSource::InstantiateUnionLayer(
                                        CPLXMLNode *psLTree,
                                        const char *pszVRTDirectory,
                                        int bUpdate,
                                        int nRecLevel)
{
    if( !EQUAL(psLTree->pszValue,"OGRVRTUnionLayer") )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Get layer name.                                                 */
/* -------------------------------------------------------------------- */
    const char *pszLayerName = CPLGetXMLValue( psLTree, "name", NULL );

    if( pszLayerName == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Missing name attribute on OGRVRTUnionLayer" );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Do we have a fixed geometry type?  If not derive from the       */
/*      source layer.                                                   */
/* -------------------------------------------------------------------- */
    const char* pszGType = CPLGetXMLValue( psLTree, "GeometryType", NULL );
    int bGlobalGeomTypeSet = FALSE;
    OGRwkbGeometryType eGlobalGeomType = wkbUnknown;
    if( pszGType != NULL )
    {
        int bError;
        bGlobalGeomTypeSet = TRUE;
        eGlobalGeomType = OGRVRTGetGeometryType(pszGType, &bError);
        if( bError )
        {
            CPLError( CE_Failure, CPLE_AppDefined,
                    "GeometryType %s not recognised.",
                    pszGType );
            return NULL;
        }
    }

/* -------------------------------------------------------------------- */
/*      Apply a spatial reference system if provided                    */
/* -------------------------------------------------------------------- */
     const char* pszLayerSRS = CPLGetXMLValue( psLTree, "LayerSRS", NULL );
     OGRSpatialReference* poGlobalSRS = NULL;
     int bGlobalSRSSet = FALSE;
     if( pszLayerSRS != NULL )
     {
         bGlobalSRSSet = TRUE;
         if( !EQUAL(pszLayerSRS,"NULL") )
         {
             OGRSpatialReference oSRS;

             if( oSRS.SetFromUserInput( pszLayerSRS ) != OGRERR_NONE )
             {
                 CPLError( CE_Failure, CPLE_AppDefined,
                           "Failed to import LayerSRS `%s'.", pszLayerSRS );
                 return NULL;
             }
             poGlobalSRS = oSRS.Clone();
         }
     }

/* -------------------------------------------------------------------- */
/*      Find field declarations.                                        */
/* -------------------------------------------------------------------- */
    OGRFieldDefn** papoFields = NULL;
    int nFields = 0;
    OGRUnionLayerGeomFieldDefn** papoGeomFields = NULL;
    int nGeomFields = 0;

    for( CPLXMLNode *psSubNode = psLTree->psChild;
         psSubNode != NULL;
         psSubNode=psSubNode->psNext )
    {
         if( psSubNode->eType != CXT_Element )
             continue;

         if( psSubNode->eType == CXT_Element && EQUAL(psSubNode->pszValue,"Field") )
         {
/* -------------------------------------------------------------------- */
/*      Field name.                                                     */
/* -------------------------------------------------------------------- */
             const char *l_pszName = CPLGetXMLValue( psSubNode, "name", NULL );
             if( l_pszName == NULL )
             {
                 CPLError( CE_Failure, CPLE_AppDefined,
                           "Unable to identify Field name." );
                 break;
             }

             OGRFieldDefn oFieldDefn( l_pszName, OFTString );

/* -------------------------------------------------------------------- */
/*      Type                                                            */
/* -------------------------------------------------------------------- */
             const char *pszArg = CPLGetXMLValue( psSubNode, "type", NULL );

             if( pszArg != NULL )
             {
                 int iType;

                 for( iType = 0; iType <= (int) OFTMaxType; iType++ )
                 {
                     if( EQUAL(pszArg,OGRFieldDefn::GetFieldTypeName(
                                   (OGRFieldType)iType)) )
                     {
                         oFieldDefn.SetType( (OGRFieldType) iType );
                         break;
                     }
                 }

                 if( iType > (int) OFTMaxType )
                 {
                     CPLError( CE_Failure, CPLE_AppDefined,
                               "Unable to identify Field type '%s'.",
                               pszArg );
                     break;
                 }
             }

/* -------------------------------------------------------------------- */
/*      Width and precision.                                            */
/* -------------------------------------------------------------------- */
             int nWidth = atoi(CPLGetXMLValue( psSubNode, "width", "0" ));
             if (nWidth < 0)
             {
                CPLError( CE_Failure, CPLE_IllegalArg,
                          "Invalid width for field %s.",
                          l_pszName );
                break;
             }
             oFieldDefn.SetWidth(nWidth);

             int nPrecision = atoi(CPLGetXMLValue( psSubNode, "precision", "0" ));
             if (nPrecision < 0 || nPrecision > 1024)
             {
                CPLError( CE_Failure, CPLE_IllegalArg,
                          "Invalid precision for field %s.",
                          l_pszName );
                break;
             }
             oFieldDefn.SetPrecision(nPrecision);

             papoFields = (OGRFieldDefn**) CPLRealloc(papoFields,
                                        sizeof(OGRFieldDefn*) * (nFields + 1));
             papoFields[nFields] = new OGRFieldDefn(&oFieldDefn);
             nFields ++;
         }

         else if( psSubNode->eType == CXT_Element &&
                  EQUAL(psSubNode->pszValue,"GeometryField") )
         {
             const char *l_pszName = CPLGetXMLValue( psSubNode, "name", NULL );
             if( l_pszName == NULL )
             {
                 CPLError( CE_Failure, CPLE_AppDefined,
                           "Unable to identify GeometryField name." );
                 break;
             }

             pszGType = CPLGetXMLValue( psSubNode, "GeometryType", NULL );
             if( pszGType == NULL && nGeomFields == 0 )
                 pszGType = CPLGetXMLValue( psLTree, "GeometryType", NULL );
             OGRwkbGeometryType eGeomType = wkbUnknown;
             int bGeomTypeSet = FALSE;
             if( pszGType != NULL )
             {
                int bError;
                eGeomType = OGRVRTGetGeometryType(pszGType, &bError);
                bGeomTypeSet = TRUE;
                if( bError || eGeomType == wkbNone )
                {
                    CPLError( CE_Failure, CPLE_AppDefined,
                            "GeometryType %s not recognised.",
                            pszGType );
                    break;
                }
             }

             const char* pszSRS = CPLGetXMLValue( psSubNode, "SRS", NULL );
             if( pszSRS == NULL && nGeomFields == 0 )
                 pszSRS = CPLGetXMLValue( psLTree, "LayerSRS", NULL );
             OGRSpatialReference* poSRS = NULL;
             int bSRSSet = FALSE;
             if( pszSRS != NULL )
             {
                 bSRSSet = TRUE;
                 if( !EQUAL(pszSRS,"NULL") )
                 {
                    OGRSpatialReference oSRS;

                    if( oSRS.SetFromUserInput( pszSRS ) != OGRERR_NONE )
                    {
                        CPLError( CE_Failure, CPLE_AppDefined,
                                "Failed to import SRS `%s'.", pszSRS );
                        break;
                    }
                    poSRS = oSRS.Clone();
                }
             }

             OGRUnionLayerGeomFieldDefn* poFieldDefn =
                    new OGRUnionLayerGeomFieldDefn(l_pszName, eGeomType);
             if( poSRS != NULL )
             {
                poFieldDefn->SetSpatialRef(poSRS);
                poSRS->Dereference();
             }
             poFieldDefn->bGeomTypeSet = bGeomTypeSet;
             poFieldDefn->bSRSSet = bSRSSet;

             const char* pszExtentXMin = CPLGetXMLValue( psSubNode, "ExtentXMin", NULL );
             const char* pszExtentYMin = CPLGetXMLValue( psSubNode, "ExtentYMin", NULL );
             const char* pszExtentXMax = CPLGetXMLValue( psSubNode, "ExtentXMax", NULL );
             const char* pszExtentYMax = CPLGetXMLValue( psSubNode, "ExtentYMax", NULL );
             if( pszExtentXMin != NULL && pszExtentYMin != NULL &&
                 pszExtentXMax != NULL && pszExtentYMax != NULL )
             {
                poFieldDefn->sStaticEnvelope.MinX = CPLAtof(pszExtentXMin);
                poFieldDefn->sStaticEnvelope.MinY = CPLAtof(pszExtentYMin);
                poFieldDefn->sStaticEnvelope.MaxX = CPLAtof(pszExtentXMax);
                poFieldDefn->sStaticEnvelope.MaxY = CPLAtof(pszExtentYMax);
             }

             papoGeomFields = (OGRUnionLayerGeomFieldDefn**) CPLRealloc(papoGeomFields,
                                        sizeof(OGRUnionLayerGeomFieldDefn*) * (nGeomFields + 1));
             papoGeomFields[nGeomFields] = poFieldDefn;
             nGeomFields ++;
         }
    }

/* -------------------------------------------------------------------- */
/*      Set Extent if provided                                          */
/* -------------------------------------------------------------------- */
    const char* pszExtentXMin = CPLGetXMLValue( psLTree, "ExtentXMin", NULL );
    const char* pszExtentYMin = CPLGetXMLValue( psLTree, "ExtentYMin", NULL );
    const char* pszExtentXMax = CPLGetXMLValue( psLTree, "ExtentXMax", NULL );
    const char* pszExtentYMax = CPLGetXMLValue( psLTree, "ExtentYMax", NULL );

    if( eGlobalGeomType != wkbNone && nGeomFields == 0 &&
        (bGlobalGeomTypeSet || bGlobalSRSSet ||
         (pszExtentXMin != NULL && pszExtentYMin != NULL &&
          pszExtentXMax != NULL && pszExtentYMax != NULL)) )
    {
        OGRUnionLayerGeomFieldDefn* poFieldDefn =
                new OGRUnionLayerGeomFieldDefn("", eGlobalGeomType);
        if( poGlobalSRS != NULL )
        {
            poFieldDefn->SetSpatialRef(poGlobalSRS);
            poGlobalSRS->Dereference();
            poGlobalSRS = NULL;
        }
        poFieldDefn->bGeomTypeSet = bGlobalGeomTypeSet;
        poFieldDefn->bSRSSet = bGlobalSRSSet;
        if( pszExtentXMin != NULL && pszExtentYMin != NULL &&
            pszExtentXMax != NULL && pszExtentYMax != NULL )
        {
            poFieldDefn->sStaticEnvelope.MinX = CPLAtof(pszExtentXMin);
            poFieldDefn->sStaticEnvelope.MinY = CPLAtof(pszExtentYMin);
            poFieldDefn->sStaticEnvelope.MaxX = CPLAtof(pszExtentXMax);
            poFieldDefn->sStaticEnvelope.MaxY = CPLAtof(pszExtentYMax);
        }

        papoGeomFields = (OGRUnionLayerGeomFieldDefn**) CPLRealloc(papoGeomFields,
                                sizeof(OGRUnionLayerGeomFieldDefn*) * (nGeomFields + 1));
        papoGeomFields[nGeomFields] = poFieldDefn;
        nGeomFields ++;
    }
    else
    {
        delete poGlobalSRS;
        poGlobalSRS = NULL;
    }

/* -------------------------------------------------------------------- */
/*      Find source layers                                              */
/* -------------------------------------------------------------------- */

    int nSrcLayers = 0;
    OGRLayer** papoSrcLayers = NULL;

    for( CPLXMLNode *psSubNode = psLTree->psChild;
         psSubNode != NULL;
         psSubNode=psSubNode->psNext )
    {
        if( psSubNode->eType != CXT_Element )
            continue;

        OGRLayer* poSrcLayer = InstantiateLayer(psSubNode, pszVRTDirectory,
                                           bUpdate, nRecLevel + 1);
        if( poSrcLayer != NULL )
        {
            papoSrcLayers = (OGRLayer**)
                CPLRealloc(papoSrcLayers, sizeof(OGRLayer*) * (nSrcLayers + 1));
            papoSrcLayers[nSrcLayers] = poSrcLayer;
            nSrcLayers ++;
        }
    }

    if( nSrcLayers == 0 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot find source layers" );
        int iField;
        for(iField = 0; iField < nFields; iField++)
            delete papoFields[iField];
        CPLFree(papoFields);
        for(iField = 0; iField < nGeomFields; iField++)
            delete papoGeomFields[iField];
        CPLFree(papoGeomFields);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Build the OGRUnionLayer.                                        */
/* -------------------------------------------------------------------- */
    OGRUnionLayer* poLayer = new OGRUnionLayer( pszLayerName,
                                                nSrcLayers,
                                                papoSrcLayers,
                                                TRUE );

/* -------------------------------------------------------------------- */
/*      Set the source layer field name attribute.                      */
/* -------------------------------------------------------------------- */
    const char* pszSourceLayerFieldName =
        CPLGetXMLValue( psLTree, "SourceLayerFieldName", NULL );
    poLayer->SetSourceLayerFieldName(pszSourceLayerFieldName);

/* -------------------------------------------------------------------- */
/*      Set the PreserveSrcFID attribute.                               */
/* -------------------------------------------------------------------- */
    int bPreserveSrcFID = FALSE;
    const char* pszPreserveFID = CPLGetXMLValue( psLTree, "PreserveSrcFID", NULL );
    if( pszPreserveFID != NULL )
        bPreserveSrcFID = CPLTestBool(pszPreserveFID);
    poLayer->SetPreserveSrcFID(bPreserveSrcFID);

/* -------------------------------------------------------------------- */
/*      Set fields                                                      */
/* -------------------------------------------------------------------- */
    FieldUnionStrategy eFieldStrategy = FIELD_UNION_ALL_LAYERS;
    const char* pszFieldStrategy = CPLGetXMLValue( psLTree, "FieldStrategy", NULL );
    if( pszFieldStrategy != NULL )
    {
        if( EQUAL(pszFieldStrategy, "FirstLayer") )
            eFieldStrategy = FIELD_FROM_FIRST_LAYER;
        else if( EQUAL(pszFieldStrategy, "Union") )
            eFieldStrategy = FIELD_UNION_ALL_LAYERS;
        else if( EQUAL(pszFieldStrategy, "Intersection") )
            eFieldStrategy = FIELD_INTERSECTION_ALL_LAYERS;
        else
        {
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Unhandled value for FieldStrategy `%s'.", pszFieldStrategy );
        }
    }
    if( nFields != 0 || nGeomFields > 1 )
    {
        if( pszFieldStrategy != NULL )
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Ignoring FieldStrategy value, because explicit Field or GeometryField is provided") ;
        eFieldStrategy = FIELD_SPECIFIED;
    }

    poLayer->SetFields(eFieldStrategy, nFields, papoFields,
                       (nGeomFields == 0 && eGlobalGeomType == wkbNone) ? -1 : nGeomFields,
                       papoGeomFields);

    for(int iField = 0; iField < nFields; iField++)
        delete papoFields[iField];
    CPLFree(papoFields);
    for(int iField = 0; iField < nGeomFields; iField++)
        delete papoGeomFields[iField];
    CPLFree(papoGeomFields);

/* -------------------------------------------------------------------- */
/*      Set FeatureCount if provided                                    */
/* -------------------------------------------------------------------- */
    const char* pszFeatureCount = CPLGetXMLValue( psLTree, "FeatureCount", NULL );
    if( pszFeatureCount != NULL )
    {
        poLayer->SetFeatureCount(atoi(pszFeatureCount));
    }

    return poLayer;
}

/************************************************************************/
/*                     InstantiateLayerInternal()                       */
/************************************************************************/

OGRLayer* OGRVRTDataSource::InstantiateLayerInternal(CPLXMLNode *psLTree,
                                                const char *pszVRTDirectory,
                                                int bUpdate,
                                                int nRecLevel)
{
/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
    if( EQUAL(psLTree->pszValue,"OGRVRTLayer") )
    {
        OGRVRTLayer* poVRTLayer = new OGRVRTLayer(this);

        if( !poVRTLayer->FastInitialize( psLTree, pszVRTDirectory, bUpdate ) )
        {
            delete poVRTLayer;
            return NULL;
        }

        return poVRTLayer;
    }
    else if( EQUAL(psLTree->pszValue,"OGRVRTWarpedLayer") && nRecLevel < 30 )
    {
        return InstantiateWarpedLayer( psLTree, pszVRTDirectory,
                                       bUpdate, nRecLevel + 1 );
    }
    else if( EQUAL(psLTree->pszValue,"OGRVRTUnionLayer") && nRecLevel < 30 )
    {
        return InstantiateUnionLayer( psLTree, pszVRTDirectory,
                                      bUpdate, nRecLevel + 1 );
    }
    else
        return NULL;
}

/************************************************************************/
/*                        OGRVRTOpenProxiedLayer()                      */
/************************************************************************/

typedef struct
{
    OGRVRTDataSource* poDS;
    CPLXMLNode *psNode;
    char       *pszVRTDirectory;
    int         bUpdate;
} PooledInitData;

static OGRLayer* OGRVRTOpenProxiedLayer(void* pUserData)
{
    PooledInitData* pData = (PooledInitData*) pUserData;
    return pData->poDS->InstantiateLayerInternal(pData->psNode,
                                            pData->pszVRTDirectory,
                                            pData->bUpdate,
                                            0);
}

/************************************************************************/
/*                     OGRVRTFreeProxiedLayerUserData()                 */
/************************************************************************/

static void OGRVRTFreeProxiedLayerUserData(void* pUserData)
{
    PooledInitData* pData = (PooledInitData*) pUserData;
    CPLFree(pData->pszVRTDirectory);
    CPLFree(pData);
}

/************************************************************************/
/*                          InstantiateLayer()                          */
/************************************************************************/

OGRLayer* OGRVRTDataSource::InstantiateLayer(CPLXMLNode *psLTree,
                                        const char *pszVRTDirectory,
                                        int bUpdate,
                                        int nRecLevel)
{
    if( poLayerPool != NULL && EQUAL(psLTree->pszValue,"OGRVRTLayer"))
    {
        PooledInitData* pData = (PooledInitData*) CPLMalloc(sizeof(PooledInitData));
        pData->poDS = this;
        pData->psNode = psLTree;
        pData->pszVRTDirectory = CPLStrdup(pszVRTDirectory);
        pData->bUpdate = bUpdate;
        return new OGRProxiedLayer(poLayerPool,
                                    OGRVRTOpenProxiedLayer,
                                    OGRVRTFreeProxiedLayerUserData,
                                    pData);
    }

    return InstantiateLayerInternal(psLTree, pszVRTDirectory,
                                    bUpdate, nRecLevel);
}

/************************************************************************/
/*                           CountOGRVRTLayers()                        */
/************************************************************************/

static int CountOGRVRTLayers(CPLXMLNode *psTree)
{
    if( psTree->eType != CXT_Element )
        return 0;

    int nCount = 0;
    if( EQUAL(psTree->pszValue, "OGRVRTLayer") )
        ++nCount;

    for( CPLXMLNode *psNode=psTree->psChild; psNode != NULL; psNode=psNode->psNext )
    {
        nCount += CountOGRVRTLayers(psNode);
    }

    return nCount;
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

int OGRVRTDataSource::Initialize( CPLXMLNode *psTreeIn, const char *pszNewName,
                                  int bUpdate )

{
    CPLAssert( nLayers == 0 );

    psTree = psTreeIn;

/* -------------------------------------------------------------------- */
/*      Set name, and capture the directory path so we can use it       */
/*      for relative datasources.                                       */
/* -------------------------------------------------------------------- */
    CPLString osVRTDirectory = CPLGetPath( pszNewName );

    pszName = CPLStrdup( pszNewName );

/* -------------------------------------------------------------------- */
/*      Look for the OGRVRTDataSource node, it might be after an        */
/*      <xml> node.                                                     */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psVRTDSXML = CPLGetXMLNode( psTree, "=OGRVRTDataSource" );
    if( psVRTDSXML == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Did not find the <OGRVRTDataSource> node in the root of the document,\n"
                  "this is not really an OGR VRT." );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Determine if we must proxy layers.                              */
/* -------------------------------------------------------------------- */
    int nOGRVRTLayerCount = CountOGRVRTLayers(psVRTDSXML);

    int nMaxSimultaneouslyOpened = atoi(CPLGetConfigOption("OGR_VRT_MAX_OPENED", "100"));
    if( nMaxSimultaneouslyOpened < 1 )
        nMaxSimultaneouslyOpened = 1;
    if( nOGRVRTLayerCount > nMaxSimultaneouslyOpened )
        poLayerPool = new OGRLayerPool(nMaxSimultaneouslyOpened);

/* -------------------------------------------------------------------- */
/*      Apply any dataset level metadata.                               */
/* -------------------------------------------------------------------- */
    oMDMD.XMLInit( psVRTDSXML, TRUE );

/* -------------------------------------------------------------------- */
/*      Look for layers.                                                */
/* -------------------------------------------------------------------- */

    for( CPLXMLNode *psLTree=psVRTDSXML->psChild; psLTree != NULL; psLTree=psLTree->psNext )
    {
        if( psLTree->eType != CXT_Element )
            continue;

/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
        OGRLayer  *poLayer = InstantiateLayer(psLTree, osVRTDirectory, bUpdate);
        if( poLayer == NULL )
            continue;

/* -------------------------------------------------------------------- */
/*      Add layer to data source layer list.                            */
/* -------------------------------------------------------------------- */
        nLayers ++;
        papoLayers = (OGRLayer **)
            CPLRealloc( papoLayers,  sizeof(OGRLayer *) * nLayers );
        papoLayers[nLayers-1] = poLayer;

        paeLayerType = (OGRLayerType*)
            CPLRealloc( paeLayerType,  sizeof(int) * nLayers );
        if( poLayerPool != NULL && EQUAL(psLTree->pszValue,"OGRVRTLayer"))
        {
            paeLayerType[nLayers - 1] = OGR_VRT_PROXIED_LAYER;
        }
        else if( EQUAL(psLTree->pszValue,"OGRVRTLayer") )
        {
            paeLayerType[nLayers - 1] = OGR_VRT_LAYER;
        }
        else
        {
            paeLayerType[nLayers - 1] = OGR_VRT_OTHER_LAYER;
        }
    }

    return TRUE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRVRTDataSource::TestCapability( const char * pszCap )
{
    if( EQUAL(pszCap,ODsCCurveGeometries) )
        return TRUE;
    return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRVRTDataSource::GetLayer( int iLayer )

{
    if( iLayer < 0 || iLayer >= nLayers )
        return NULL;

    return papoLayers[iLayer];
}

/************************************************************************/
/*                         AddForbiddenNames()                          */
/************************************************************************/

void OGRVRTDataSource::AddForbiddenNames(const char* pszOtherDSName)
{
    aosOtherDSNameSet.insert(pszOtherDSName);
}

/************************************************************************/
/*                         IsInForbiddenNames()                         */
/************************************************************************/

int OGRVRTDataSource::IsInForbiddenNames(const char* pszOtherDSName)
{
    return aosOtherDSNameSet.find(pszOtherDSName) != aosOtherDSNameSet.end();
}

/************************************************************************/
/*                             GetFileList()                             */
/************************************************************************/

char **OGRVRTDataSource::GetFileList()
{
    CPLStringList oList;
    oList.AddString( GetName() );
    for(int i=0; i<nLayers; i++ )
    {
        OGRLayer* poLayer = papoLayers[i];
        OGRVRTLayer* poVRTLayer = NULL;
        switch( paeLayerType[nLayers - 1] )
        {
            case OGR_VRT_PROXIED_LAYER:
                poVRTLayer = (OGRVRTLayer*) ((OGRProxiedLayer*)poLayer)->GetUnderlyingLayer();
                break;
            case OGR_VRT_LAYER:
                poVRTLayer = (OGRVRTLayer*) poLayer;
                break;
            default:
                break;
        }
        if( poVRTLayer != NULL )
        {
            GDALDataset* poSrcDS = poVRTLayer->GetSrcDataset();
            if( poSrcDS != NULL )
            {
                char** papszFileList = poSrcDS->GetFileList();
                char** papszIter = papszFileList;
                for(; papszIter != NULL && *papszIter != NULL; papszIter++ )
                {
                    if( oList.FindString(*papszIter) < 0 )
                        oList.AddString(*papszIter);
                }
                CSLDestroy(papszFileList);
            }
        }
    }
    return oList.StealList();
}
