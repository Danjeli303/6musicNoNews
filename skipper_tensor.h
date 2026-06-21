////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER ****                           //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#ifndef SKIPPER_TENSOR_H
#define SKIPPER_TENSOR_H

#include "skipper.h"

int read_tensor_file (tensor_array tensor, char *filename);
int local_tensor_file (tensor_array tensor, unsigned char *compressed_tensor, int compressed_size);

#endif
