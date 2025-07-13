
177. 第N高的薪水

SQL Schema
Pandas Schema
表: Employee

+-------------+------+
| Column Name | Type |
+-------------+------+
| id          | int  |
| salary      | int  |
+-------------+------+
在 SQL 中，id 是该表的主键。
该表的每一行都包含有关员工工资的信息。
 

查询 Employee 表中第 n 高的工资。如果没有第 n 个最高工资，查询结果应该为 null 。

查询结果格式如下所示。

 

示例 1:

输入: 
Employee table:
+----+--------+
| id | salary |
+----+--------+
| 1  | 100    |
| 2  | 200    |
| 3  | 300    |
+----+--------+
n = 2
输出: 
+------------------------+
| getNthHighestSalary(2) |
+------------------------+
| 200                    |
+------------------------+
示例 2:

输入: 
Employee 表:
+----+--------+
| id | salary |
+----+--------+
| 1  | 100    |
+----+--------+
n = 2
输出: 
+------------------------+
| getNthHighestSalary(2) |
+------------------------+
| null                   |
+------------------------+


// solution 1
CREATE FUNCTION getNthHighestSalary(N INT) RETURNS INT
BEGIN
  RETURN (
      # Write your MySQL query statement below.
select max(salary)
from
(
    select dense_rank() over(order by salary desc) num, salary
    from employee
) temp
where num=N


 );
END

// solution 2
CREATE FUNCTION getNthHighestSalary(N INT) RETURNS INT
BEGIN
declare m INT;
set m = N-1;
  RETURN (
      # Write your MySQL query statement below.
      select ifnull
      (
          (
              select distinct salary
              from employee
              order by salary desc
              limit 1 offset m
          ), null
      )
  );
END

