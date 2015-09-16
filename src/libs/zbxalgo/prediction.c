/*
** Zabbix
** Copyright (C) 2001-2015 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"

#include "zbxalgo.h"

#define ZBX_MATH_OK	SUCCEED
#define ZBX_MATH_FAIL	FAIL

#define DB_INFINITY	(1e12-1e-4)
#define ERROR_CODE	-1.0

#define ZBX_VALID_MATRIX(m)		(0 < m->rows && 0 < m->columns && NULL != m->elements)
#define ZBX_MATRIX_EL(m, row, col)	(m->elements[(row) * m->columns + (col)])
#define ZBX_MATRIX_ROW(m, row)		(m->elements + (row) * m->columns)

typedef struct
{
	int	rows;
	int	columns;
	double	*elements;
}
zbx_matrix_t;

typedef enum
{
	FIT_LINEAR,
	FIT_POLYNOMIAL,
	FIT_EXPONENTIAL,
	FIT_LOGARITHMIC,
	FIT_POWER,
	FIT_INVALID
}
zbx_fit_t;

typedef enum
{
	MODE_VALUE,
	MODE_MAX,
	MODE_MIN,
	MODE_DELTA,
	MODE_AVG,
	MODE_INVALID
}
zbx_mode_t;

static int	zbx_matrix_struct_alloc(zbx_matrix_t **pm)
{
	if (NULL == (*pm = (zbx_matrix_t *)zbx_malloc(*pm, sizeof(zbx_matrix_t))))
		return ZBX_MATH_FAIL;

	(*pm)->rows = 0;
	(*pm)->columns = 0;
	(*pm)->elements = NULL;

	return ZBX_MATH_OK;
}

static int	zbx_matrix_alloc(zbx_matrix_t *m, int rows, int columns, char **error)
{
	if (0 >= rows || 0 >= columns)
	{
		*error = zbx_strdup(*error, "invalid matrix dimensions");
		return ZBX_MATH_FAIL;
	}

	m->rows = rows;
	m->columns = columns;

	if(NULL == (m->elements = (double *)zbx_malloc(m->elements, sizeof(double) * rows * columns)))
		return ZBX_MATH_FAIL;

	return ZBX_MATH_OK;
}

static void	zbx_matrix_free(zbx_matrix_t *m)
{
	if (NULL != m)
		zbx_free(m->elements);

	zbx_free(m);
}

static int	zbx_matrix_copy(zbx_matrix_t *dest, zbx_matrix_t *src, char **error)
{
	if (!ZBX_VALID_MATRIX(src))
	{
		*error = zbx_strdup(*error, "source matrix is not valid");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_alloc(dest, src->rows, src->columns, error))
		return ZBX_MATH_FAIL;

	memcpy(dest->elements, src->elements, sizeof(double) * src->rows * src->columns);
	return ZBX_MATH_OK;
}

static int	zbx_identity_matrix(zbx_matrix_t *m, int n, char **error)
{
	int	i, j;

	if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, n, error))
		return ZBX_MATH_FAIL;

	for (i = 0; i < n; ++i)
		for (j = 0; j < n; ++j)
			ZBX_MATRIX_EL(m, i, j) = (i == j ? 1.0 : 0.0);

	return ZBX_MATH_OK;
}

static int	zbx_transpose_matrix(zbx_matrix_t *m, zbx_matrix_t *r, char **error)
{
	int	i, j;

	if (!ZBX_VALID_MATRIX(m))
	{
		*error = zbx_strdup(*error, "matrix to transpose is not valid");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_alloc(r, m->columns, m->rows, error))
		return ZBX_MATH_FAIL;

	for (i = 0; i < r->rows; ++i)
		for (j = 0; j < r->columns; ++j)
			ZBX_MATRIX_EL(r, i, j) = ZBX_MATRIX_EL(m, j, i);

	return ZBX_MATH_OK;
}

static void	zbx_matrix_swap_rows(zbx_matrix_t *m, int r1, int r2)
{
	double	tmp;
	int	i;

	for (i = 0; i < m->columns; ++i)
	{
		tmp = ZBX_MATRIX_EL(m, r1, i);
		ZBX_MATRIX_EL(m, r1, i) = ZBX_MATRIX_EL(m, r2, i);
		ZBX_MATRIX_EL(m, r2, i) = tmp;
	}
}

static void	zbx_matrix_divide_row_by(zbx_matrix_t *m, int row, double denominator)
{
	int	i;

	for (i = 0; i < m->columns; ++i)
		ZBX_MATRIX_EL(m, row, i) /= denominator;
}

static void	zbx_matrix_add_rows_with_factor(zbx_matrix_t *m, int dest, int src, double factor)
{
	int	i;

	for (i = 0; i < m->columns; ++i)
		ZBX_MATRIX_EL(m, dest, i) += ZBX_MATRIX_EL(m, src, i) * factor;
}

static int	zbx_inverse_matrix(zbx_matrix_t *m, zbx_matrix_t *r, char **error)
{
	zbx_matrix_t	*l = NULL;
	double		pivot, factor, det;
	int		i, j, k, n, res;

	if (!ZBX_VALID_MATRIX(m))
	{
		*error = zbx_strdup(*error, "matrix to invert is not valid");
		return ZBX_MATH_FAIL;
	}

	if (m->rows != m->columns)
	{
		*error = zbx_strdup(*error, "matrix to invert is not square");
		return ZBX_MATH_FAIL;
	}

	n = m->rows;

	if (1 == n)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(r, 1, 1, error))
			return ZBX_MATH_FAIL;

		if (0.0 == ZBX_MATRIX_EL(m, 0, 0))
		{
			*error = zbx_strdup(*error, "matrix is singular");
			res = ZBX_MATH_FAIL;
			goto out;
		}

		ZBX_MATRIX_EL(r, 0, 0) = 1.0 / ZBX_MATRIX_EL(m, 0, 0);
		return ZBX_MATH_OK;
	}

	if (2 == n)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(r, 2, 2, error))
			return ZBX_MATH_FAIL;

		if (0.0 == (det = ZBX_MATRIX_EL(m, 0, 0) * ZBX_MATRIX_EL(m, 1, 1) -
				ZBX_MATRIX_EL(m, 0, 1) * ZBX_MATRIX_EL(m, 1, 0)))
		{
			*error = zbx_strdup(*error, "matrix is singular");
			res = ZBX_MATH_FAIL;
			goto out;
		}

		ZBX_MATRIX_EL(r, 0, 0) = ZBX_MATRIX_EL(m, 1, 1) / det;
		ZBX_MATRIX_EL(r, 0, 1) = -ZBX_MATRIX_EL(m, 0, 1) / det;
		ZBX_MATRIX_EL(r, 1, 0) = -ZBX_MATRIX_EL(m, 1, 0) / det;
		ZBX_MATRIX_EL(r, 1, 1) = ZBX_MATRIX_EL(m, 0, 0) / det;
		return ZBX_MATH_OK;
	}

	if (ZBX_MATH_OK != zbx_identity_matrix(r, n, error))
		return ZBX_MATH_FAIL;

	if (ZBX_MATH_OK != (res = zbx_matrix_struct_alloc(&l)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_matrix_copy(l, m, error)))
		goto out;

	/* Gauss-Jordan elimination with partial (row) pivoting */
	for (i = 0; i < n; ++i)
	{
		k = i;
		pivot = ZBX_MATRIX_EL(l, i, i);

		for (j = i; j < n; ++j)
		{
			if (fabs(ZBX_MATRIX_EL(l, j, i)) > fabs(pivot))
			{
				k = j;
				pivot = ZBX_MATRIX_EL(l, j, i);
			}
		}

		if (0.0 == pivot)
		{
			*error = zbx_strdup(*error, "matrix is singular");
			res = ZBX_MATH_FAIL;
			goto out;
		}

		if (k != i)
		{
			zbx_matrix_swap_rows(l, i, k);
			zbx_matrix_swap_rows(r, i, k);
		}

		for (j = i + 1; j < n; ++j)
		{
			if (0.0 != (factor = -ZBX_MATRIX_EL(l, j, i) / ZBX_MATRIX_EL(l, i, i)))
			{
				zbx_matrix_add_rows_with_factor(l, j, i, factor);
				zbx_matrix_add_rows_with_factor(r, j, i, factor);
			}
		}
	}

	for (i = n - 1; i > 0; --i)
	{
		for (j = 0; j < i; ++j)
		{
			if (0.0 != (factor = -ZBX_MATRIX_EL(l, j, i) / ZBX_MATRIX_EL(l, i, i)))
			{
				zbx_matrix_add_rows_with_factor(l, j, i, factor);
				zbx_matrix_add_rows_with_factor(r, j, i, factor);
			}
		}
	}

	for (i = 0; i < n; ++i)
		zbx_matrix_divide_row_by(r, i, ZBX_MATRIX_EL(l, i, i));

	res = ZBX_MATH_OK;
out:
	zbx_matrix_free(l);
	return res;
}

static int	zbx_matrix_mult(zbx_matrix_t *left, zbx_matrix_t *right, zbx_matrix_t *result, char **error)
{
	double	element;
	int	i, j, k;

	if (!ZBX_VALID_MATRIX(left) || !ZBX_VALID_MATRIX(right))
	{
		*error = zbx_strdup(*error, "matrices to multiplicate are not valid");
		return ZBX_MATH_FAIL;
	}

	if (left->columns != right->rows)
	{
		*error = zbx_strdup(*error, "matrices to multiplicate are of incompatible dimensions");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_alloc(result, left->rows, right->columns, error))
		return ZBX_MATH_FAIL;

	for (i = 0; i < result->rows; ++i)
	{
		for (j = 0; j < result->columns; ++j)
		{
			element = 0;

			for (k = 0; k < left->columns; ++k)
				element += ZBX_MATRIX_EL(left, i, k) * ZBX_MATRIX_EL(right, k, j);

			ZBX_MATRIX_EL(result, i, j) = element;
		}
	}

	return ZBX_MATH_OK;
}

static int	zbx_least_squares(zbx_matrix_t *independent, zbx_matrix_t *dependent, zbx_matrix_t *coefficients,
		char **error)
{
	/*                         |<----------to_be_inverted---------->|                                          */
	/* coefficients = inverse( transpose( independent ) * independent ) * transpose( independent ) * dependent */
	/*                |<------------------left_part------------------>|   |<-----------right_part----------->| */
	/*           we change order of matrix multiplication to reduce operation count and memory usage           */
	zbx_matrix_t	*independent_transposed = NULL, *to_be_inverted = NULL, *left_part = NULL, *right_part = NULL;
	int		res;

	if (ZBX_MATH_OK != zbx_matrix_struct_alloc(&independent_transposed) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&to_be_inverted) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&left_part) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&right_part))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	if (ZBX_MATH_OK != (res = zbx_transpose_matrix(independent, independent_transposed, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_matrix_mult(independent_transposed, independent, to_be_inverted, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_inverse_matrix(to_be_inverted, left_part, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_matrix_mult(independent_transposed, dependent, right_part, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_matrix_mult(left_part, right_part, coefficients, error)))
		goto out;

out:
	zbx_matrix_free(independent_transposed);
	zbx_matrix_free(to_be_inverted);
	zbx_matrix_free(left_part);
	zbx_matrix_free(right_part);
	return res;
}

static int	zbx_fill_dependent(double *x, int n, zbx_fit_t fit, zbx_matrix_t *m, char **error)
{
	int	i;

	if (FIT_LINEAR == fit || FIT_POLYNOMIAL == fit || FIT_LOGARITHMIC == fit)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, 1, error))
			return ZBX_MATH_FAIL;

		for (i = 0; i < n; ++i)
			ZBX_MATRIX_EL(m, i, 0) = x[i];
	}
	else if (FIT_EXPONENTIAL == fit || FIT_POWER == fit)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, 1, error))
			return ZBX_MATH_FAIL;

		for (i = 0; i < n; ++i)
		{
			if (0.0 >= x[i])
			{
				*error = zbx_strdup(*error, "data contains negative or zero values");
				return ZBX_MATH_FAIL;
			}

			ZBX_MATRIX_EL(m, i, 0) = log(x[i]);
		}
	}

	return ZBX_MATH_OK;
}

static int	zbx_fill_independent(double *t, int n, zbx_fit_t fit, zbx_matrix_t *m, char **error)
{
	double		element;
	int		i, j;
	unsigned int	k;

	if (FIT_LINEAR == fit || FIT_EXPONENTIAL == fit)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, 2, error))
			return ZBX_MATH_FAIL;

		for (i = 0; i < n; ++i)
		{
			ZBX_MATRIX_EL(m, i, 0) = 1.0;
			ZBX_MATRIX_EL(m, i, 1) = t[i];
		}
	}
	else if (FIT_LOGARITHMIC == fit || FIT_POWER == fit)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, 2, error))
			return ZBX_MATH_FAIL;

		for (i = 0; i < n; ++i)
		{
			ZBX_MATRIX_EL(m, i, 0) = 1.0;
			ZBX_MATRIX_EL(m, i, 1) = log(t[i]);
		}
	}
	else if (FIT_POLYNOMIAL == fit)
	{
		if (SUCCEED != is_uint_range(fit + strlen("polynomial"), &k, 1, 6))
		{
			*error = zbx_strdup(*error, "polynomial degree is invalid");
			return ZBX_MATH_FAIL;
		}

		if (k > n - 1)
			k = n - 1;

		if (ZBX_MATH_OK != zbx_matrix_alloc(m, n, k+1, error))
			return ZBX_MATH_FAIL;

		for (i = 0; i < n; ++i)
		{
			element = 1.0;

			for (j = 0; j < k; ++j)
			{
				ZBX_MATRIX_EL(m, i, j) = element;
				element *= t[i];
			}

			ZBX_MATRIX_EL(m, i, k) = element;
		}
	}

	return ZBX_MATH_OK;
}

static int	zbx_regression(double *t, double *x, int n, zbx_fit_t fit, zbx_matrix_t *coefficients, char **error)
{
	zbx_matrix_t	*independent = NULL, *dependent = NULL;
	int		res;

	if (ZBX_MATH_OK != zbx_matrix_struct_alloc(&independent) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&dependent))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	if (ZBX_MATH_OK != (res = zbx_fill_independent(t, n, fit, independent, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_fill_dependent(x, n, fit, dependent, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_least_squares(independent, dependent, coefficients, error)))
		goto out;

out:
	zbx_matrix_free(independent);
	zbx_matrix_free(dependent);
	return res;
}

static double	zbx_polynomial_value(double t, zbx_matrix_t *coefficients)
{
	double	pow = 1.0, res = 0.0;
	int	i;

	for (i = 0; i < coefficients->rows; ++i, pow *= t)
		res += ZBX_MATRIX_EL(coefficients, i, 0) * pow;

	return res;
}

static double	zbx_polynomial_antiderivative(double t, zbx_matrix_t *coefficients)
{
	double	pow = t, res = 0.0;
	int	i;

	for (i = 0; i < coefficients->rows; ++i, pow *= t)
		res += ZBX_MATRIX_EL(coefficients, i, 0) * pow / (i + 1);

	return res;
}

static int	zbx_derive_polynomial(zbx_matrix_t *polynomial, zbx_matrix_t *derivative, char **error)
{
	int	i;

	if (!ZBX_VALID_MATRIX(polynomial))
	{
		*error = zbx_strdup(*error, "polynomial to derive is not valid");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_alloc(derivative, (polynomial->rows > 1 ? polynomial->rows - 1 : 1), 1, error))
		return ZBX_MATH_FAIL;

	for (i = 1; i < polynomial->rows; ++i)
		ZBX_MATRIX_EL(derivative, i - 1, 0) = ZBX_MATRIX_EL(polynomial, i, 0) * i;

	if (1 == i)
		ZBX_MATRIX_EL(derivative, 0, 0) = 0.0;

	return ZBX_MATH_OK;
}

#define Re(z)	(z)[0]
#define Im(z)	(z)[1]

#define ZBX_COMPLEX_MULT(z1, z2, tmp)			\
do							\
{							\
	Re(tmp) = Re(z1) * Re(z2) - Im(z1) * Im(z2);	\
	Im(tmp) = Re(z1) * Im(z2) + Im(z1) * Re(z2);	\
	Re(z1) = Re(tmp);				\
	Im(z1) = Im(tmp);				\
}							\
while(0)

#define ZBX_MATH_EPSILON	(1.0e-6)

#define ZBX_MAX_ITERATIONS	200

static int	zbx_polynomial_roots(zbx_matrix_t *coefficients, zbx_matrix_t *roots, char **error)
{
	zbx_matrix_t	*denominator_multiplicands = NULL, *updates = NULL;
	double		z[2], mult[2], denominator[2], zpower[2], polynomial[2], highest_degree_coefficient,
			lower_bound, upper_bound, radius, max_update, min_distance, residual, temp;
	int		i, j, degree, first_nonzero, res, iteration = 0, roots_ok = 0, root_init = 0;

	if (!ZBX_VALID_MATRIX(coefficients))
	{
		*error = zbx_strdup(*error, "polynomial to find roots of is not valid");
		return ZBX_MATH_FAIL;
	}

	degree = coefficients->rows - 1;
	highest_degree_coefficient = ZBX_MATRIX_EL(coefficients, degree, 0);

	while (0.0 == highest_degree_coefficient && 0 < degree)
		highest_degree_coefficient = ZBX_MATRIX_EL(coefficients, --degree, 0);

	if (0 == degree)
	{
		if (0.0 == highest_degree_coefficient)
		{
			*error = zbx_strdup(*error, "every number is a root, cannot return anything meaningful");
			return ZBX_MATH_FAIL;
		}

		return ZBX_MATH_OK;
	}

	if (1 == degree)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(roots, 1, 2, error))
			return ZBX_MATH_FAIL;

		Re(ZBX_MATRIX_ROW(roots, 0)) = -ZBX_MATRIX_EL(coefficients, 0, 0) / ZBX_MATRIX_EL(coefficients, 1, 0);
		Im(ZBX_MATRIX_ROW(roots, 0)) = 0.0;

		return ZBX_MATH_OK;
	}

	if (2 == degree)
	{
		if (ZBX_MATH_OK != zbx_matrix_alloc(roots, 2, 2, error))
			return ZBX_MATH_FAIL;

		if (0.0 < (temp = ZBX_MATRIX_EL(coefficients, 1, 0) * ZBX_MATRIX_EL(coefficients, 1, 0) -
				4 * ZBX_MATRIX_EL(coefficients, 2, 0) * ZBX_MATRIX_EL(coefficients, 0, 0)))
		{
			temp = (0 < ZBX_MATRIX_EL(coefficients, 1, 0) ?
					-ZBX_MATRIX_EL(coefficients, 1, 0) - sqrt(temp) :
					-ZBX_MATRIX_EL(coefficients, 1, 0) + sqrt(temp));
			Re(ZBX_MATRIX_ROW(roots, 0)) = 0.5 * temp / ZBX_MATRIX_EL(coefficients, 2, 0);
			Re(ZBX_MATRIX_ROW(roots, 1)) = 2.0 * ZBX_MATRIX_EL(coefficients, 0, 0) / temp;
			Im(ZBX_MATRIX_ROW(roots, 0)) = Im(ZBX_MATRIX_ROW(roots, 1)) = 0.0;
		}
		else
		{
			Re(ZBX_MATRIX_ROW(roots, 0)) = Re(ZBX_MATRIX_ROW(roots, 1)) =
					-0.5 * ZBX_MATRIX_EL(coefficients, 1, 0) / ZBX_MATRIX_EL(coefficients, 2, 0);
			Im(ZBX_MATRIX_ROW(roots, 0)) = -(Im(ZBX_MATRIX_ROW(roots, 1)) = 0.5 * sqrt(-temp)) /
					ZBX_MATRIX_EL(coefficients, 2, 0);
		}

		return ZBX_MATH_OK;
	}

	if (ZBX_MATH_OK != zbx_matrix_struct_alloc(&denominator_multiplicands) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&updates))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	if (ZBX_MATH_OK != zbx_matrix_alloc(roots, degree, 2, error) ||
			ZBX_MATH_OK != zbx_matrix_alloc(denominator_multiplicands, degree, 2, error) ||
			ZBX_MATH_OK != zbx_matrix_alloc(updates, degree, 2, error))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	/* if n lower coefficients are zeros, zero is a root of multiplicity n */
	for (first_nonzero = 0; 0.0 == ZBX_MATRIX_EL(coefficients, first_nonzero, 0); ++first_nonzero)
		Re(ZBX_MATRIX_ROW(roots, first_nonzero)) = Im(ZBX_MATRIX_ROW(roots, first_nonzero)) = 0.0;

	/* compute bounds for the roots */
	upper_bound = lower_bound = 1.0;

	for (i = first_nonzero; i < degree; ++i)
	{
		if (upper_bound < fabs(ZBX_MATRIX_EL(coefficients, i, 0) / highest_degree_coefficient))
			upper_bound = fabs(ZBX_MATRIX_EL(coefficients, i, 0) / highest_degree_coefficient);

		if (lower_bound < fabs(ZBX_MATRIX_EL(coefficients, i + 1, 0) /
				ZBX_MATRIX_EL(coefficients, first_nonzero, 0)))
			lower_bound = fabs(ZBX_MATRIX_EL(coefficients, i + 1, 0) /
					ZBX_MATRIX_EL(coefficients, first_nonzero, 0));
	}

	radius = lower_bound = 1.0 / lower_bound;

	/* Weierstrass (Durand-Kerner) method */
	while (ZBX_MAX_ITERATIONS >= ++iteration && !roots_ok)
	{
		if (0 == root_init)
		{
			if (radius <= upper_bound)
			{
				for (i = 0; i < degree - first_nonzero; ++i)
				{
					Re(ZBX_MATRIX_ROW(roots, i)) = radius * cos((2.0 * M_PI * (i + 0.25)) /
							(degree - first_nonzero));
					Im(ZBX_MATRIX_ROW(roots, i)) = radius * sin((2.0 * M_PI * (i + 0.25)) /
							(degree - first_nonzero));
				}

				radius *= 2.0;
			}
			else
			{
				root_init = 1;
			}
		}

		roots_ok = 1;
		max_update = 0.0;
		min_distance = HUGE_VAL;

		for (i = first_nonzero; i < degree; ++i)
		{
			Re(z) = Re(ZBX_MATRIX_ROW(roots, i));
			Im(z) = Im(ZBX_MATRIX_ROW(roots, i));

			/* subtract from z every one of denominator_multiplicands and multiplicate them */
			Re(denominator) = highest_degree_coefficient;
			Im(denominator) = 0.0;

			for (j = first_nonzero; j < degree; ++j)
			{
				if (j == i)
					continue;

				temp = (ZBX_MATRIX_EL(roots, i, 0) - ZBX_MATRIX_EL(roots, j, 0)) *
						(ZBX_MATRIX_EL(roots, i, 0) - ZBX_MATRIX_EL(roots, j, 0)) +
						(ZBX_MATRIX_EL(roots, i, 1) - ZBX_MATRIX_EL(roots, j, 1)) *
						(ZBX_MATRIX_EL(roots, i, 1) - ZBX_MATRIX_EL(roots, j, 1));
				if (temp < min_distance)
					min_distance = temp;

				Re(ZBX_MATRIX_ROW(denominator_multiplicands, j)) = Re(z) - Re(ZBX_MATRIX_ROW(roots, j));
				Im(ZBX_MATRIX_ROW(denominator_multiplicands, j)) = Im(z) - Im(ZBX_MATRIX_ROW(roots, j));
				ZBX_COMPLEX_MULT(denominator, ZBX_MATRIX_ROW(denominator_multiplicands, j), mult);
			}

			/* calculate complex value of polynomial for z */
			Re(zpower) = 1.0;
			Im(zpower) = 0.0;
			Re(polynomial) = ZBX_MATRIX_EL(coefficients, first_nonzero, 0);
			Im(polynomial) = 0.0;

			for (j = first_nonzero + 1; j <= degree; ++j)
			{
				ZBX_COMPLEX_MULT(zpower, z, mult);
				Re(polynomial) += Re(zpower) * ZBX_MATRIX_EL(coefficients, j, 0);
				Im(polynomial) += Im(zpower) * ZBX_MATRIX_EL(coefficients, j, 0);
			}

			/* check how good root approximation is */
			residual = fabs(Re(polynomial)) + fabs(Im(polynomial));
			roots_ok = roots_ok && (ZBX_MATH_EPSILON > residual);

			/* divide polynomial value by denominator */
			if (0.0 != (temp = Re(denominator) * Re(denominator) + Im(denominator) * Im(denominator)))
			{
				Re(ZBX_MATRIX_ROW(updates, i)) = (Re(polynomial) * Re(denominator) +
						Im(polynomial) * Im(denominator)) / temp;
				Im(ZBX_MATRIX_ROW(updates, i)) = (Im(polynomial) * Re(denominator) -
						Re(polynomial) * Im(denominator)) / temp;
			}
			else /* Denominator is zero iff two or more root approximations are equal. */
				/* Since root approximations are initially different their equality means that they */
				/* converged to a multiple root (hopefully) and no updates are required in this case. */
			{
				Re(ZBX_MATRIX_ROW(updates, i)) = Im(ZBX_MATRIX_ROW(updates, i)) = 0.0;
			}

			temp = ZBX_MATRIX_EL(updates, i, 0) * ZBX_MATRIX_EL(updates, i, 0) +
					ZBX_MATRIX_EL(updates, i, 1) * ZBX_MATRIX_EL(updates, i, 1);

			if (temp > max_update)
				max_update = temp;
		}

		if (max_update > radius * radius && 0 == root_init)
			continue;
		else
			root_init = 1;

		for (i = first_nonzero; i < degree; ++i)
		{
			Re(ZBX_MATRIX_ROW(roots, i)) -= Re(ZBX_MATRIX_ROW(updates, i));
			Im(ZBX_MATRIX_ROW(roots, i)) -= Im(ZBX_MATRIX_ROW(updates, i));
		}
	}

	if (0 == roots_ok)
	{
		*error = zbx_strdup(*error, "polynomial root finding problem is ill-defined");
		res = ZBX_MATH_FAIL;
	}
	else
	{
		res = ZBX_MATH_OK;
	}
out:
	zbx_matrix_free(denominator_multiplicands);
	zbx_matrix_free(updates);
	return res;
}

#undef ZBX_MAX_ITERATIONS

#undef Re
#undef Im

static int	zbx_polynomial_minmax(double now, double time, zbx_mode_t mode, zbx_matrix_t *coefficients,
		double *result, char **error)
{
	zbx_matrix_t	*derivative = NULL, *derivative_roots = NULL;
	double		min, max, tmp;
	int		i, res;

	if (!ZBX_VALID_MATRIX(coefficients))
	{
		*error = zbx_strdup(*error, "polynomial to find extrema of is not valid");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_struct_alloc(&derivative) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&derivative_roots))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	if (ZBX_MATH_OK != (res = zbx_derive_polynomial(coefficients, derivative, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_polynomial_roots(derivative, derivative_roots, error)))
		goto out;

	/* choose min and max among now, now + time and derivative roots inbetween (these are potential local extrema) */
	/* we ignore imaginary part of roots, this means that more calculations will be made, */
	/* but result will not be affected and we wont need a boundary on minimal imaginary part that differs from zero */

	min = zbx_polynomial_value(now, coefficients);
	tmp = zbx_polynomial_value(now + time, coefficients);

	if (tmp < min)
	{
		max = min;
		min = tmp;
	}
	else
	{
		max = tmp;
	}

	for (i = 0; i < derivative_roots->rows; ++i)
	{
		tmp = ZBX_MATRIX_EL(derivative_roots, i, 0);

		if (tmp < now || tmp > now + time)
			continue;

		tmp = zbx_polynomial_value(tmp, coefficients);

		if (tmp < min)
			min = tmp;
		else if (tmp > max)
			max = tmp;
	}

	if (MODE_MAX == mode)
		*result = max;
	else if (MODE_MIN == mode)
		*result = min;
	else /* if (MODE_DELTA == mode) */
		*result = max - min;

out:
	zbx_matrix_free(derivative);
	zbx_matrix_free(derivative_roots);
	return res;
}

static int	zbx_polynomial_timeleft(double now, double threshold, zbx_matrix_t *coefficients, double *result,
		char **error)
{
	zbx_matrix_t	*shifted_coefficients = NULL, *roots = NULL;
	double		tmp;
	int		i, res, no_root = 1;

	if (!ZBX_VALID_MATRIX(coefficients))
	{
		*error = zbx_strdup(*error, "polynomial is not valid");
		return ZBX_MATH_FAIL;
	}

	if (ZBX_MATH_OK != zbx_matrix_struct_alloc(&shifted_coefficients) ||
			ZBX_MATH_OK != zbx_matrix_struct_alloc(&roots))
	{
		res = ZBX_MATH_FAIL;
		goto out;
	}

	if (ZBX_MATH_OK != (res = zbx_matrix_copy(shifted_coefficients, coefficients, error)))
		goto out;

	ZBX_MATRIX_EL(shifted_coefficients, 0, 0) -= threshold;

	if (ZBX_MATH_OK != (res = zbx_polynomial_roots(shifted_coefficients, roots, error)))
		goto out;

	/* choose the closest root right from now or set result to -1 otherwise */
	/* if zbx_polynomial_value(tmp) is not close enough to zero it must be a complex root and must be skipped */

	for (i = 0; i < roots->rows; ++i)
	{
		tmp = ZBX_MATRIX_EL(roots, i, 0);

		if (no_root)
		{
			if (tmp > now && ZBX_MATH_EPSILON > fabs(zbx_polynomial_value(tmp, shifted_coefficients)))
			{
				no_root = 0;
				*result = tmp;
			}
		}
		else if (now < tmp && tmp < *result &&
				ZBX_MATH_EPSILON > fabs(zbx_polynomial_value(tmp, shifted_coefficients)))
		{
			*result = tmp;
		}
	}

	if (no_root)
		*result = -1.0;
	else
		*result -= now;

out:
	zbx_matrix_free(shifted_coefficients);
	zbx_matrix_free(roots);
	return res;
}

#undef ZBX_MATH_EPSILON

static int	zbx_calculate_value(double t, zbx_matrix_t *coefficients, zbx_fit_t fit, double *value, char **error)
{
	if (!ZBX_VALID_MATRIX(coefficients))
	{
		*error = zbx_strdup(*error, "coefficients are not valid");
		return ZBX_MATH_FAIL;
	}

	if (FIT_LINEAR == fit)
		*value = ZBX_MATRIX_EL(coefficients, 0, 0) + ZBX_MATRIX_EL(coefficients, 1, 0) * t;
	else if (FIT_POLYNOMIAL == fit)
		*value = zbx_polynomial_value(t, coefficients);
	else if (FIT_EXPONENTIAL == fit)
		*value = exp(ZBX_MATRIX_EL(coefficients, 0, 0) + ZBX_MATRIX_EL(coefficients, 1, 0) * t);
	else if (FIT_LOGARITHMIC == fit)
		*value = ZBX_MATRIX_EL(coefficients, 0, 0) + ZBX_MATRIX_EL(coefficients, 1, 0) * log(t);
	else if (FIT_POWER == fit)
		*value = exp(ZBX_MATRIX_EL(coefficients, 0, 0) + ZBX_MATRIX_EL(coefficients, 1, 0) * log(t));

	return ZBX_MATH_OK;
}

zbx_fit_t	zbx_fit_code(char *fit_str)
{
	if ('\0' == *fit || 0 == strcmp(fit, "linear"))
		return = FIT_LINEAR;

	if (0 == strncmp(fit, "polynomial", strlen("polynomial")))
		return FIT_POLYNOMIAL;

	if (0 == strcmp(fit, "exponential"))
		return FIT_EXPONENTIAL;

	if (0 == strcmp(fit, "logarithmic"))
		return FIT_LOGARITHMIC;

	if (0 == strcmp(fit, "power"))
		return FIT_POWER;

	return FIT_INVALID;
}

zbx_mode_t	zbx_mode_code(char *mode_str)
{
	if ('\0' == *mode || 0 == strcmp(mode, "value"))
		return MODE_VALUE;

	if (0 == strcmp(mode, "max"))
		return MODE_MAX;

	if (0 == strcmp(mode, "min"))
		return MODE_MIN;

	if (0 == strcmp(mode, "delta"))
		return MODE_DELTA;

	if (0 == strcmp(mode, "avg"))
		return MODE_AVG;

	return MODE_INVALID;
}

#define ZBX_IS_NAN(x)	(x != x)

int	zbx_forecast(double *t, double *x, int n, double now, double time, char *fit_str, char *mode_str,
	double *result, char **error)
{
	zbx_matrix_t	*coefficients = NULL;
	zbx_fit_t	fit;
	zbx_mode_t	mode;
	double		left, right;
	int		res;

	if (FIT_INVALID == (fit = zbx_fit_code(fit_str)))
	{
		*error = zbx_strdup(*error, "invalid 'fit' parameter");
		return ZBX_MATH_FAIL;
	}

	if (MODE_INVALID == (mode = zbx_mode_code(mode_str)))
	{
		*error = zbx_strdup(*error, "invalid 'mode' parameter");
		return ZBX_MATH_FAIL;
	}

	if (1 == n)
	{
		if (MODE_VALUE == mode || MODE_MAX == mode || MODE_MIN == mode || MODE_AVG == mode)
			*result = x[0];
		else if (MODE_DELTA == mode)
			*result = 0.0;

		return ZBX_MATH_OK;
	}

	if (ZBX_MATH_OK != (res = zbx_matrix_struct_alloc(&coefficients)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_regression(t, x, n, fit, coefficients, error)))
		goto out;

	if (MODE_VALUE == mode)
	{
		res = zbx_calculate_value(now + time, coefficients, fit, result, error);
		goto out;
	}

	if (0.0 == time)
	{
		if (MODE_MAX == mode || MODE_MIN == mode || MODE_AVG == mode)
		{
			res = zbx_calculate_value(now + time, coefficients, fit, result, error);
		}
		else if (MODE_DELTA == mode)
		{
			*result = 0.0;
			res = ZBX_MATH_OK;
		}

		goto out;
	}

	if (FIT_LINEAR == fit || FIT_EXPONENTIAL == fit || FIT_LOGARITHMIC == fit || FIT_POWER == fit)
	{
		/* fit is monotone, therefore maximum and minimum are either at now or at now + time */
		if (ZBX_MATH_OK != zbx_calculate_value(now, coefficients, fit, &left, error) ||
				ZBX_MATH_OK != zbx_calculate_value(now + time, coefficients, fit, &right, error))
		{
			res = ZBX_MATH_FAIL;
			goto out;
		}

		if (MODE_MAX == mode)
		{
			*result = (left > right ? left : right);
		}
		else if (MODE_MIN == mode)
		{
			*result = (left < right ? left : right);
		}
		else if (MODE_DELTA == mode)
		{
			*result = (left > right ? left - right : right - left);
		}
		else if (MODE_AVG == mode)
		{
			if (FIT_LINEAR == fit)
			{
				*result = 0.5 * (left + right);
			}
			else if (FIT_EXPONENTIAL == fit)
			{
				*result = (right - left) / time / ZBX_MATRIX_EL(coefficients, 1, 0);
			}
			else if (FIT_LOGARITHMIC == fit)
			{
				*result = right + ZBX_MATRIX_EL(coefficients, 1, 0) *
						(log(1.0 + time / now) * now / time - 1.0);
			}
			else if (FIT_POWER == fit)
			{
				if (-1.0 != ZBX_MATRIX_EL(coefficients, 1, 0))
					*result = (right * (now + time) - left * now) / time /
							(ZBX_MATRIX_EL(coefficients, 1, 0) + 1.0);
				else
					*result = exp(ZBX_MATRIX_EL(coefficients, 0, 0)) * log(1.0 + time / now) / time;
			}
		}

		res = ZBX_MATH_OK;
	}

	if (FIT_POLYNOMIAL == fit)
	{
		if (MODE_MAX == mode || MODE_MIN == mode || MODE_DELTA == mode)
			res = zbx_polynomial_minmax(now, time, mode, coefficients, result, error);

		if (MODE_AVG == mode)
		{
			*result = (zbx_polynomial_antiderivative(now + time, coefficients) -
					zbx_polynomial_antiderivative(now, coefficients)) / time;
			res = ZBX_MATH_OK;
		}
	}

out:
	if (ZBX_MATH_OK == res)
	{
		if (ZBX_IS_NAN(*result))
			*result = ERROR_CODE;
		else if (DB_INFINITY < *result)
			*result = DB_INFINITY;
		else if (-DB_INFINITY > *result)
			*result = -DB_INFINITY;
	}

	zbx_matrix_free(coefficients);
	return res;
}

int	zbx_timeleft(double *t, double *x, int n, double now, double threshold, char *fit_str, double *result, char **error)
{
	zbx_matrix_t	*coefficients = NULL;
	zbx_fit_t	fit;
	double		current;
	int		res;

	if (FIT_INVALID == (fit = zbx_fit_code(fit_str)))
	{
		*error = zbx_strdup(*error, "invalid 'fit' parameter");
		return ZBX_MATH_FAIL;
	}

	if ((FIT_EXPONENTIAL == fit || FIT_POWER == fit) && 0.0 >= threshold)
	{
		*error = zbx_strdup(*error, "exponential and power functions are always positive");
		return ZBX_MATH_FAIL;
	}

	if (1 == n)
	{
		*result = (x[0] == threshold ? 0.0 : DB_INFINITY);
		return ZBX_MATH_OK;
	}

	if (ZBX_MATH_OK != (res = zbx_matrix_struct_alloc(&coefficients)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_regression(t, x, n, fit, coefficients, error)))
		goto out;

	if (ZBX_MATH_OK != (res = zbx_calculate_value(now, coefficients, fit, &current, error)))
	{
		goto out;
	}
	else if (current == threshold)
	{
		*result = 0.0;
		goto out;
	}

	if (FIT_LINEAR == fit)
		*result = (threshold - ZBX_MATRIX_EL(coefficients, 0, 0)) / ZBX_MATRIX_EL(coefficients, 1, 0) - now;
	else if (FIT_POLYNOMIAL == fit)
		res = zbx_polynomial_timeleft(now, threshold, coefficients, result, error);
	else if (FIT_EXPONENTIAL == fit)
		*result = (log(threshold) - ZBX_MATRIX_EL(coefficients, 0, 0)) / ZBX_MATRIX_EL(coefficients, 1, 0)
				- now;
	else if (FIT_LOGARITHMIC == fit)
		*result = exp((threshold - ZBX_MATRIX_EL(coefficients, 0, 0)) / ZBX_MATRIX_EL(coefficients, 1, 0))
				- now;
	else if (FIT_POWER == fit)
		*result = exp((log(threshold) - ZBX_MATRIX_EL(coefficients, 0, 0)) / ZBX_MATRIX_EL(coefficients, 1, 0))
				- now;

	if (ZBX_IS_NAN(*result))
		*result = ERROR_CODE;
	else if (0.0 > *result || DB_INFINITY < *result)
		*result = DB_INFINITY;

out:
	zbx_matrix_free(coefficients);
	return res;
}

#undef ZBX_IS_NAN

#undef ZBX_VALID_MATRIX
#undef ZBX_MATRIX_EL
#undef ZBX_MATRIX_ROW

#undef DB_INFINITY
#undef ERROR_CODE

#undef ZBX_MATH_OK
#undef ZBX_MATH_FAIL

