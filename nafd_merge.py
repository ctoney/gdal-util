# nafd_merge.py - merge path/row data into larger mosaic
# Chris Toney, christoney@fs.fed.us

# v2, 20151118
#	- add option for reproject source files or not

from osgeo import gdal
from osgeo.gdalconst import *
from osgeo import osr
import os
import csv
import subprocess
import glob

# for dist attrib maps:
#in_files = "/nobackup/nadf3/attribution/data/national/outputs/pXXXrXXX/w2pXXXrXXX_TSNational_TSallAgent_VCTDF_TSCalValTodd_pred_agent.img"
# for VCT maps 2010:
#in_files = "/nobackup/nadf3/share/FourthNationalRun/VCTstacks/pXXXrXXX/mmu_outputs/w2pXXXrXXX_2010*_distbMap_v2"
# for VCT maps 2010 Albers:
in_files = "/nobackup/nadf3/share/FourthNationalRun/VCTstacks_albers/pXXXrXXX/w2pXXXrXXX_2010*_distbMap_v2_albers"

in_nodata = 0
reproject = False
out_proj4 = "+proj=aea +lat_1=29.5 +lat_2=45.5 +lat_0=23 +lon_0=-96 +x_0=0 +y_0=0 +datum=NAD83 +units=m +no_defs"
ref_file = "/nobackup/nadf3/share/VCT_National_Mosaic_v5/annual_us_mosaic_combine_mmu2_2010"
out_dir = "/nobackup/nadf3/attribution/data/national/outputs/mosaic_test/"
out_file = "vct_2010_conus_test_albers.img"
out_fmt = "HFA"
out_fill_value = 0
keep_pathrow_reprojects = True
path_row_reproject_fmt = 'VRT'

# get the path/rows and process each one
f = open('pathrows_ul_to_lr.txt')
pathrows = f.read().splitlines()
f.close()

errors = {}

for pathrow in pathrows:
	this_in_file = in_files.replace("pXXXrXXX", pathrow)

	# for dist attrib maps:
	#if (not(os.path.isfile(this_in_file))):
	#	this_in_file = this_in_file.replace("TSNational", "TSWest")
	# for VCT maps:	
	try:
		this_in_file = glob.glob(this_in_file)[0]
	except:
		continue

	# temp: fix up these file names
	path_row_out_file = os.path.join(out_dir, (pathrow + ".vrt"))

	if reproject:
		arglist = ['gdalwarp']
		arglist.append('-t_srs')
		arglist.append(out_proj4)
		arglist.append('-tr')
		arglist.append('30')
		arglist.append('30')
		arglist.append('-of')
		arglist.append(path_row_reproject_fmt)
		arglist.append(this_in_file)
		arglist.append(path_row_out_file)
	else:
		arglist = ['gdal_translate']
		arglist.append('-of')
		arglist.append('VRT')
		arglist.append(this_in_file)
		arglist.append(path_row_out_file)

	ret = subprocess.call(arglist)
	if (ret != 0):
		errors[pathrow] = ret
		print("gdalwarp for " + pathrow + " returned " + str(ret))

# write a log file
f = open('merge_reproj_errors.txt', 'wb')
writer = csv.writer(f)
writer.writerow(['pathrow','ret_code'])
for pr, e in errors.iteritems():
	writer.writerow([pr]+[e])
f.close()

# create the output file
print("Creating the output file...")
ds = gdal.Open(ref_file)
xsize = ds.RasterXSize
ysize = ds.RasterYSize
gt = ds.GetGeoTransform()
if not gt is None:
	xmin = gt[0]
	ymax = gt[3]
	xmax = xmin + (xsize * gt[1])
	ymin = ymax + (ysize * gt[5])
ds = None

out_driver = gdal.GetDriverByName(out_fmt)
out_filename = os.path.join(out_dir, out_file)
ds_out = out_driver.Create(out_filename, xsize, ysize, 1, GDT_Byte)
srs = osr.SpatialReference()
srs.ImportFromProj4(out_proj4)
ds_out.SetProjection(srs.ExportToWkt())
ds_out.SetGeoTransform(gt)
ds_out.GetRasterBand(1).Fill(out_fill_value)
ds_out = None

print("gdal_merge...")
# merge in the pathrows
for pathrow in pathrows:
	# temp: fix up these file names as above
	path_row_file = os.path.join(out_dir, (pathrow + ".vrt"))

	arglist = ['python']
	arglist.append('gdal_merge.py')
	arglist.append('-v')
	arglist.append('-o')
	arglist.append(out_filename)
	arglist.append('-n')
	arglist.append(str(in_nodata))
	arglist.append(path_row_file)
	print(arglist)
	ret = subprocess.call(arglist)
	print(str(ret))

print(out_filename)
print("Done.")

