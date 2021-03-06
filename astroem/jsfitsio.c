/*
 *
 * cfitsio_extra.c -- extra routines for javascript version
 *
 */

/*
Notes:
passing arrays and passing by reference:
http://stackoverflow.com/questions/17883799/how-to-handle-passing-returning-array-pointers-to-emscripten-compiled-code
https://groups.google.com/forum/#!topic/emscripten-discuss/JDaNHIRQ_G4
*/
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "fitsio.h"
#if EM
#include <emscripten.h>
#endif

#define IDIM 2
#define IFILE "mem://";
#define MFILE "foo"

#define max(a,b) (a>=b?a:b)
#define min(a,b) (a<=b?a:b)

// the emscripten heap is about 1Gb, so we have to place limits on memory
// somewhat arbitrarily, this size allows for a 10600 x 10600 4-byte image
#define MAX_MEMORY 450000000
static int max_memory = MAX_MEMORY;

// ffhist3: same as ffhist2, but does not close the original file,
// and/or replace the original file pointer 
fitsfile *ffhist3(fitsfile *fptr, /* I - ptr to table with X and Y cols*/
           char *outfile,    /* I - name for the output histogram file      */
           int imagetype,    /* I - datatype for image: TINT, TSHORT, etc   */
           int naxis,        /* I - number of axes in the histogram image   */
           char colname[4][FLEN_VALUE],   /* I - column names               */
           double *minin,     /* I - minimum histogram value, for each axis */
           double *maxin,     /* I - maximum histogram value, for each axis */
           double *binsizein, /* I - bin size along each axis               */
           char minname[4][FLEN_VALUE], /* I - optional keywords for min    */
           char maxname[4][FLEN_VALUE], /* I - optional keywords for max    */
           char binname[4][FLEN_VALUE], /* I - optional keywords for binsize */
           double weightin,        /* I - binning weighting factor          */
           char wtcol[FLEN_VALUE], /* I - optional keyword or col for weight*/
           int recip,              /* I - use reciprocal of the weight?     */
           char *selectrow,        /* I - optional array (length = no. of   */
                             /* rows in the table).  If the element is true */
                             /* then the corresponding row of the table will*/
                             /* be included in the histogram, otherwise the */
                             /* row will be skipped.  Ingnored if *selectrow*/
                             /* is equal to NULL.                           */
           int *status)
{
    fitsfile *histptr;
    int   bitpix, colnum[4], wtcolnum;
    long haxes[4];
    float amin[4], amax[4], binsize[4],  weight;

    if (*status > 0)
        return(NULL);

    if (naxis > 4)
    {
        ffpmsg("histogram has more than 4 dimensions");
	*status = BAD_DIMEN;
        return(NULL);
    }

    /* reset position to the correct HDU if necessary */
    if ((fptr)->HDUposition != ((fptr)->Fptr)->curhdu)
        ffmahd(fptr, ((fptr)->HDUposition) + 1, NULL, status);

    if (imagetype == TBYTE)
        bitpix = BYTE_IMG;
    else if (imagetype == TSHORT)
        bitpix = SHORT_IMG;
    else if (imagetype == TINT)
        bitpix = LONG_IMG;
    else if (imagetype == TFLOAT)
        bitpix = FLOAT_IMG;
    else if (imagetype == TDOUBLE)
        bitpix = DOUBLE_IMG;
    else{
        *status = BAD_DATATYPE;
        return(NULL);
    }
    
    /*    Calculate the binning parameters:    */
    /*   columm numbers, axes length, min values,  max values, and binsizes.  */

    if (fits_calc_binning(
      fptr, naxis, colname, minin, maxin, binsizein, minname, maxname, binname,
      colnum, haxes, amin, amax, binsize, status) > 0)
    {
       ffpmsg("failed to determine binning parameters");
        return(NULL);
    }
 
    /* get the histogramming weighting factor, if any */
    if (*wtcol)
    {
        /* first, look for a keyword with the weight value */
        if (fits_read_key(fptr, TFLOAT, wtcol, &weight, NULL, status) )
        {
            /* not a keyword, so look for column with this name */
            *status = 0;

            /* get the column number in the table */
            if (ffgcno(fptr, CASEINSEN, wtcol, &wtcolnum, status) > 0)
            {
               ffpmsg(
               "keyword or column for histogram weights doesn't exist: ");
               ffpmsg(wtcol);
               return(NULL);
            }

            weight = FLOATNULLVALUE;
        }
    }
    else
        weight = (float) weightin;

    if (weight <= 0. && weight != FLOATNULLVALUE)
    {
        ffpmsg("Illegal histogramming weighting factor <= 0.");
	*status = URL_PARSE_ERROR;
        return(NULL);
    }

    if (recip && weight != FLOATNULLVALUE)
       /* take reciprocal of weight */
       weight = (float) (1.0 / weight);

    /* size of histogram is now known, so create temp output file */
    if (fits_create_file(&histptr, outfile, status) > 0)
    {
        ffpmsg("failed to create temp output file for histogram");
        return(NULL);
    }

    /* create output FITS image HDU */
    if (ffcrim(histptr, bitpix, naxis, haxes, status) > 0)
    {
        ffpmsg("failed to create output histogram FITS image");
        return(NULL);
    }

    /* copy header keywords, converting pixel list WCS keywords to image WCS form */
    if (fits_copy_pixlist2image(fptr, histptr, 9, naxis, colnum, status) > 0)
    {
        ffpmsg("failed to copy pixel list keywords to new histogram header");
        return(NULL);
    }

    /* if the table columns have no WCS keywords, then write default keywords */
    fits_write_keys_histo(fptr, histptr, naxis, colnum, status);
    
    /* update the WCS keywords for the ref. pixel location, and pixel size */
    fits_rebin_wcs(histptr, naxis, amin, binsize,  status);      
    
    /* now compute the output image by binning the column values */
    if (fits_make_hist(fptr, histptr, bitpix, naxis, haxes, colnum, amin, amax,
        binsize, weight, wtcolnum, recip, selectrow, status) > 0)
    {
        ffpmsg("failed to calculate new histogram values");
        return(NULL);
    }
              
    return(histptr);
}

// gotoFITSHDU: try to go to a reasonable HDU if the primary is useless
// we look for specified extensions and if not found, go to hdu #2
// this is how xray binary tables are imaged automatically
fitsfile *gotoFITSHDU(fitsfile *fptr, char *extlist, int *hdutype, int *status){
  int hdunum, naxis, thdutype, gotext;
  char *ext, *textlist;
  // if this is the primary array and it does not contain an image,
  // try to move to something more reasonble
  fits_get_hdu_num(fptr, &hdunum); *status = 0;
  fits_get_img_dim(fptr, &naxis, status); *status = 0;
  if( (hdunum == 1) && (naxis == 0) ){ 
    // look through the extension list
    if( extlist ){
      gotext = 0;
      textlist = (char *)strdup(extlist);
      for(ext=(char *)strtok(textlist, " "); ext != NULL; 
	  ext=(char *)strtok(NULL," ")){
	fits_movnam_hdu(fptr, ANY_HDU, ext, 0, status);
	if( *status == 0 ){
	  gotext = 1;
	  break;
	} else {
	  *status = 0;
	}
      }
      free(textlist);      
    }
    if( !gotext ){
      // if all else fails, move to extension #2 and hope for the best
      fits_movabs_hdu(fptr, 2, &thdutype, status);
    }
  }
  fits_get_hdu_type(fptr, hdutype, status);
  return fptr;
}

// openFITSFile: open a FITS file for reading and go to a useful HDU
//
fitsfile *openFITSFile(char *ifile, char *extlist, int *hdutype, int *status){
  fitsfile *fptr;
  // open fits file
  fits_open_file(&fptr, ifile, READONLY, status);
  // bail out if there is an error at this point
  if( *status ){
    return NULL;
  }
  return gotoFITSHDU(fptr, extlist, hdutype, status);
}

// openFITSMem: open a FITS memory buffer for reading and go to a useful HDU
fitsfile *openFITSMem(void **buf, size_t *buflen, char *extlist, 
		      int *hdutype, int *status){
  fitsfile *fptr;
  // open fits file
  fits_open_memfile(&fptr, MFILE, READONLY, buf, buflen, 0, NULL, status);
  // bail out if there is an error at this point
  if( *status ){
    return NULL;
  }
  return gotoFITSHDU(fptr, extlist, hdutype, status);
}

// getImageToArray: extract a sub-section from an image HDU, return array
void *getImageToArray(fitsfile *fptr, int *dims, double *cens, 
		      int *odim1, int *odim2, int *bitpix, int *status){
  int naxis=IDIM;
  int xcen, ycen, dim1, dim2, type;
  void *obuf;
  long totpix, totbytes;
  long naxes[IDIM], fpixel[IDIM], lpixel[IDIM], inc[IDIM]={1,1};

  // get image dimensions and type
  fits_get_img_dim(fptr, &naxis, status);
  fits_get_img_size(fptr, IDIM, naxes, status);
  fits_get_img_type(fptr, bitpix, status);

  // get limits of extracted section
  if( dims && dims[0] && dims[1] ){
    dim1 = min(dims[0], naxes[0]);
    dim2 = min(dims[1], naxes[1]);
    // read image section
    if( cens ){
      xcen = cens[0];
      ycen = cens[1];
    } else {
      xcen = dim1/2;
      ycen = dim2/2;
    }
    fpixel[0] = (int)(xcen - ((dim1+1)/2) + 1);
    fpixel[1] = (int)(ycen - ((dim2+1)/2) + 1);
    lpixel[0] = (int)(xcen + (dim1/2));
    lpixel[1] = (int)(ycen + (dim2/2));
  } else {
    // read entire image
    fpixel[0] = 1;
    fpixel[1] = 1;
    lpixel[0] = naxes[0];
    lpixel[1] = naxes[1];
  }
  if( fpixel[0] < 1 ){
    fpixel[0] = 1;
  }
  if( fpixel[0] > naxes[0] ){
    fpixel[0] = naxes[0];
  }
  if( fpixel[1] < 1 ){
    fpixel[1] = 1;
  }
  if( fpixel[1] > naxes[1] ){
    fpixel[1] = naxes[1];
  }
  // section dimensions
  *odim1 = lpixel[0] - fpixel[0] + 1;
  *odim2 = lpixel[1] - fpixel[1] + 1;
  totpix = *odim1 * *odim2;
  // allocate space for the pixel array
  switch(*bitpix){
    case 8:
      type = TBYTE;
      totbytes = totpix * sizeof(char);
      break;
    case 16:
      type = TSHORT;
      totbytes = totpix * sizeof(short);
      break;
    case -16:
      type = TUSHORT;
      totbytes = totpix * sizeof(unsigned short);
      break;
    case 32:
      type = TINT;
      totbytes = totpix * sizeof(int);
      break;
    case 64:
      type = TLONGLONG;
      totbytes = totpix * sizeof(long long);
      break;
    case -32:
      type = TFLOAT;
      totbytes = totpix * sizeof(float);
      break;
    case -64:
      type = TDOUBLE;
      totbytes = totpix * sizeof(double);
      break;
  default:
    return NULL;
  }
  // sanity check on memory limits
  if( totbytes > max_memory ){
    return NULL;
  }
  // try to allocate that much memory
  if(!(obuf = (void *)malloc(totbytes))){
    return NULL;
  }
  /* read the image section */
  fits_read_subset(fptr, type, fpixel, lpixel, inc, 0, obuf, 0, status);
  // return pixel buffer (and section dimensions)
  return obuf;
}

// filterTableToImage: filter a binary table, create a temp image
fitsfile *filterTableToImage(fitsfile *fptr, char *filter, char **cols,
			     int *dims, double *cens, int bin, int *status){
  int i, dim1, dim2;
  int imagetype=TINT, naxis=IDIM, recip=0;
  int tstatus;
  long nirow, norow;
  float weight=1;
  float xcen, ycen;
  double dvalue;
  double minin[IDIM], maxin[IDIM], binsizein[IDIM];
  char comment[81];
  char *rowselect=NULL;
  char *outfile=IFILE;
  char wtcol[FLEN_VALUE];
  char colname[IDIM][FLEN_VALUE];
  char minname[IDIM][FLEN_VALUE];
  char maxname[IDIM][FLEN_VALUE];
  char binname[IDIM][FLEN_VALUE];
  int colnum[IDIM];
  long haxes[IDIM];
  float amin[IDIM];
  float amax[IDIM];
  float binsize[IDIM];
  fitsfile *ofptr;

  // set up defaults
  if( !bin ) bin = 1;
  wtcol[0] = '\0';
  if( cols && cols[0] && cols[1] ){
    strcpy(colname[0], cols[0]);
    strcpy(colname[1], cols[1]);
  } else {
    colname[0][0] = '\0';
    colname[1][0] = '\0';
  }
  for(i=0; i<IDIM; i++){
    minin[i] = DOUBLENULLVALUE;
    maxin[i] = DOUBLENULLVALUE;
    binsizein[i] = (double)bin;
    minname[i][0] = '\0';
    maxname[i][0] = '\0';
    binname[i][0] = '\0';
  }
  // get total number of rows in input file
  fits_get_num_rows(fptr, &nirow, status);
  // and allocate memory for selected rows array
  rowselect = malloc(nirow+1);
  // filter the input file and generate selected rows array
  if( filter && *filter ){
    fits_find_rows(fptr, filter, 0, nirow, &norow, rowselect,  status);
  } else {
    for(i=0; i<nirow+1; i++){
      rowselect[i] = TRUE;
    }
  }
  // get binning parameters so we can know the image dims for these cols
  // and from that, get the default center of the image
  fits_calc_binning(fptr, naxis, colname, minin, maxin, binsizein, 
		    minname, maxname, binname,
		    colnum, haxes, amin, amax, binsize, status);
  // why truncate to int? otherwise, cfitsio is 0.5 pixels off from js9 ...
  xcen = (int)(amax[0] - amin[0])/2;
  ycen = (int)(amax[1] - amin[1])/2;
  dim1 = haxes[0];
  dim2 = haxes[0];
  // get limits of extracted section
  if( dims && dims[0] && dims[1] ){
    // read image section
    if( cens && cens[0] && cens[1] ){
      xcen = cens[0];
      ycen = cens[1];
      dim1 = dims[0];
      dim2 = dims[1];
    } else {
      if( haxes[0] >= dims[0] ){
	dim1 = dims[0];
      }
      if( haxes[1] >= dims[1] ){
	dim2 = dims[1];
      }
    }
    minin[0] = (int)(xcen - ((dim1+1)/2) + 1);
    minin[1] = (int)(ycen - ((dim2+1)/2) + 1);
    maxin[0] = (int)(xcen + (dim1/2));
    maxin[1] = (int)(ycen + (dim2/2));
  }
  // make 2D section histogram from selected rows
  ofptr = ffhist3(fptr, outfile, imagetype, naxis, colname, 
		  minin, maxin, binsizein, minname, maxname, binname,
		  weight, wtcol, recip, rowselect, status);

  // update/add LTM and LTV header params
  dvalue = 0.0; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTV1", &dvalue, comment, &tstatus); 
  dvalue = (dim1 / 2) - xcen; tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTV1", &dvalue, comment, &tstatus);

  dvalue = 0.0; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTV2", &dvalue, comment, &tstatus); 
  dvalue = (dim2 / 2) - ycen; tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTV2", &dvalue, comment, &tstatus);

  dvalue = 1.0 / bin; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTM1_1", &dvalue, comment, &tstatus); 
  tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTM1_1", &dvalue, comment, &tstatus);

  dvalue = 0.0; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTM1_2", &dvalue, comment, &tstatus); 
  tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTM1_2", &dvalue, comment, &tstatus);

  dvalue = 0.0; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTM2_1", &dvalue, comment, &tstatus); 
  tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTM2_1", &dvalue, comment, &tstatus);

  dvalue = 1.0 / bin; *comment = '\0'; tstatus = 0;
  fits_read_key(fptr, TDOUBLE, "LTM2_2", &dvalue, comment, &tstatus); 
  tstatus = 0;
  fits_update_key(ofptr, TDOUBLE, "LTM2_2", &dvalue, comment, &tstatus);

  // return the center and dims used
  if( dims ){
    dims[0] = dim1;
    dims[1] = dim2;
  }
  if( cens ){
    cens[0] = xcen;
    cens[1] = ycen;
  }
  // free up space
  if( rowselect ) free(rowselect);
  // return new file pointer
  return ofptr;
}

void getHeaderToString(fitsfile *fptr, char **cardstr, int *ncard, int *status){
  fits_convert_hdr2str(fptr, 1, NULL, 0, cardstr, ncard, status);
}


// closeFITSFile: close a FITS file or memory object
void closeFITSFile(fitsfile *fptr, int *status){
  fits_close_file(fptr, status);
}

// maxFITSMemory: set limit of size of memory available for a FITS image
int maxFITSMemory(int limit){
  int old = max_memory;
  // if 0, don't set, just return current
  if( limit ){
    max_memory = limit;
  }
  // return prev value
  return old;
}
