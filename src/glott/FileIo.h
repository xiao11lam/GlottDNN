#ifndef FILEIO_H_
#define FILEIO_H_

#include "definitions.h"

int ReadWavFile(const char *fname, gsl::vector *signal, Param *params);
int WriteWavFile(const std::string &filename, const gsl::vector &signal, const int &fs);
int ReadGslVector(const std::string &filename, const DataType format, gsl::vector *vector_ptr);
int ReadGslMatrix(const std::string &filename, const DataType format, const size_t n_rows,  gsl::matrix *matrix_ptr);
int WriteGslVector(const std::string &filename, const DataType &format, const gsl::vector &vector);
int WriteGslMatrix(const std::string &filename, const DataType &format, const gsl::matrix &mat);
int ReadSynthesisData(const char *basename, Param *params, SynthesisData *data);

int ReadFileFloat(const std::string &fname_str, float **file_data, size_t *n_read);
int WriteFileFloat(const std::string &fname_str, const float *data, const size_t &n_values);

#endif /* FILEIO_H_ */
