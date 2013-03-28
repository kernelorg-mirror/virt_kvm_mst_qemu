/*
 * BER Output Visitor header
 *
 * Copyright IBM, Corp. 2013
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Stefan Berger     <stefanb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef BER_OUTPUT_VISITOR_H
#define BER_OUTPUT_VISITOR_H

#include "qapi/visitor.h"
#include "qapi/ber.h"

typedef struct BEROutputVisitor BEROutputVisitor;

BEROutputVisitor *ber_output_visitor_new(QEMUFile *,
                                         BERLengthEncoding le);
void ber_output_visitor_cleanup(BEROutputVisitor *v);

Visitor *ber_output_get_visitor(BEROutputVisitor *v);

BERLengthEncoding ber_output_set_length_encoding(BEROutputVisitor *v,
                                                 BERLengthEncoding le);

#endif
