/******************************************************************
//
//
//  Copyright(C) 2020, Institute for Defense Analyses
//  4850 Mark Center Drive, Alexandria, VA; 703-845-2500
// 
//
//  All rights reserved.
//  
//   This file is a part of Bale.  For license information see the
//   LICENSE file in the top level directory of the distribution.
//  
// 
 *****************************************************************/ 

/*! \file triangle.h
 * \brief Demo application that counts the number of triangles
 *  in a graph. The graph is stored as a lower triangular sparse matrix.
 */
#ifndef TRIANGLE_H
#define TRIANGLE_H

/*
#include <libgetput.h>
#include <exstack.h>
#include <convey.h>
*/
#include "spmat.h"
#include <locale.h>
#include <buddy.h>

double   triangle_buddy_push(int64_t *count, int64_t *sr, sparsemat_t * L, sparsemat_t * U, int64_t alg);

#endif
